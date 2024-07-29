// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#include "fboss/agent/test/AgentEnsembleTest.h"
#include <folly/gen/Base.h>
#include <optional>
#include "fboss/agent/AgentConfig.h"
#include "fboss/agent/CommonInit.h"
#include "fboss/agent/HwAsicTable.h"
#include "fboss/agent/Main.h"
#include "fboss/agent/SwitchIdScopeResolver.h"
#include "fboss/agent/hw/switch_asics/HwAsic.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/test/utils/QosTestUtils.h"
#include "fboss/qsfp_service/lib/QsfpClient.h"

namespace {
int argCount{0};
char** argVec{nullptr};
} // unnamed namespace

DEFINE_bool(run_forever, false, "run the test forever");
DEFINE_bool(run_forever_on_failure, false, "run the test forever on failure");

DECLARE_string(config);
DECLARE_bool(disable_looped_fabric_ports);

namespace facebook::fboss {

void AgentEnsembleTest::setupAgentEnsemble() {
  setVersionInfo();
  gflags::ParseCommandLineFlags(&argCount, &argVec, false);
  // TODO(Elangovan) add support for production features

  auto config = fbossCommonInit(argCount, argVec);
  // Reset any global state being tracked in singletons
  // Each test then sets up its own state as needed.
  folly::SingletonVault::singleton()->destroyInstances();
  folly::SingletonVault::singleton()->reenableInstances();

  setCmdLineFlagOverrides();

  AgentEnsembleSwitchConfigFn initialConfigFn =
      [this](const AgentEnsemble& ensemble) { return initialConfig(ensemble); };

  agentEnsemble_ = createAgentEnsemble(
      initialConfigFn,
      true /* disablelinkstatetoggler*/,
      platformConfigFn_,
      (HwSwitch::FeaturesDesired::PACKET_RX_DESIRED |
       HwSwitch::FeaturesDesired::LINKSCAN_DESIRED),
      false /* failHwCallsOnWarmboot*/);
  utilCreateDir(getAgentTestDir());
  XLOG(DBG2) << "Agent has been setup and ready for the test";
}

void AgentEnsembleTest::setCmdLineFlagOverrides() const {
  // Looped ports are the common case in tests
  FLAGS_disable_looped_fabric_ports = false;
}

void AgentEnsembleTest::TearDown() {
  if (FLAGS_run_forever ||
      (::testing::Test::HasFailure() && FLAGS_run_forever_on_failure)) {
    runForever();
  }
  tearDownAgentEnsemble();
  bool gracefulExit = !::testing::Test::HasFailure();
}

void AgentEnsembleTest::tearDownAgentEnsemble(bool doWarmboot) {
  if (!agentEnsemble_) {
    return;
  }

  if (::testing::Test::HasFailure()) {
    // TODO: Collect Info and dump counters
  }
  if (doWarmboot) {
    agentEnsemble_->gracefulExit();
  }
  agentEnsemble_.reset();
}

cfg::SwitchConfig AgentEnsembleTest::initialConfig(
    const AgentEnsemble& ensemble) const {
  auto agentConf = AgentConfig::fromDefaultFile();
  return agentConf->thrift.sw();
}

// We are now returning a map of portId to port stats. Need to modify callers
// accordingly
std::map<PortID, HwPortStats> AgentEnsembleTest::getPortStats(
    const std::vector<PortID>& ports) const {
  std::map<PortID, HwPortStats> portStats;
  checkWithRetry(
      [&portStats, &ports, this]() {
        portStats = getSw()->getHwPortStats(ports);
        // Check collect timestamp is valid
        for (const auto& [portId, portStats] : portStats) {
          if (*portStats.timestamp__ref() ==
              hardware_stats_constants::STAT_UNINITIALIZED()) {
            return false;
          }
        }
        return !portStats.empty();
      },
      120,
      std::chrono::milliseconds(1000),
      " fetch port stats");
  return portStats;
}

SwSwitch* AgentEnsembleTest::getSw() const {
  return agentEnsemble_->getSw();
}

AgentEnsemble* AgentEnsembleTest::getAgentEnsemble() const {
  return agentEnsemble_.get();
}

const std::shared_ptr<SwitchState> AgentEnsembleTest::getProgrammedState()
    const {
  return getAgentEnsemble()->getProgrammedState();
}

void AgentEnsembleTest::resolveNeighbor(
    PortDescriptor portDesc,
    const folly::IPAddress& ip,
    folly::MacAddress mac) {
  // TODO support agg ports as well.
  CHECK(portDesc.isPhysicalPort());
  auto port = getProgrammedState()->getPorts()->getNodeIf(portDesc.phyPortID());
  auto vlan = port->getVlans().begin()->first;
  if (ip.isV4()) {
    resolveNeighbor(portDesc, ip.asV4(), vlan, mac);
  } else {
    resolveNeighbor(portDesc, ip.asV6(), vlan, mac);
  }
}

template <typename AddrT>
void AgentEnsembleTest::resolveNeighbor(
    PortDescriptor port,
    const AddrT& ip,
    VlanID vlanId,
    folly::MacAddress mac) {
  auto resolveNeighborFn = [=,
                            this](const std::shared_ptr<SwitchState>& state) {
    auto outputState{state->clone()};
    auto vlan = outputState->getVlans()->getNode(vlanId);
    auto nbrTable = vlan->template getNeighborEntryTable<AddrT>()->modify(
        vlanId, &outputState);
    if (nbrTable->getEntryIf(ip)) {
      nbrTable->updateEntry(
          ip, mac, port, vlan->getInterfaceID(), NeighborState::REACHABLE);
    } else {
      nbrTable->addEntry(ip, mac, port, vlan->getInterfaceID());
    }
    return outputState;
  };
  getSw()->updateStateBlocking("resolve nbr", resolveNeighborFn);
}

void AgentEnsembleTest::runForever() const {
  XLOG(DBG2) << "AgentEnsembleTest run forever...";
  while (true) {
    sleep(1);
    XLOG_EVERY_MS(DBG2, 5000) << "AgentEnsembleTest running forever";
  }
}

bool AgentEnsembleTest::waitForSwitchStateCondition(
    std::function<bool(const std::shared_ptr<SwitchState>&)> conditionFn,
    uint32_t retries,
    std::chrono::duration<uint32_t, std::milli> msBetweenRetry) const {
  auto newState = sw()->getState();
  while (retries--) {
    if (conditionFn(newState)) {
      return true;
    }
    std::this_thread::sleep_for(msBetweenRetry);
    newState = getProgrammedState();
  }
  XLOG(DBG3) << "Awaited state condition was never satisfied";
  return false;
}

void AgentEnsembleTest::setPortStatus(PortID portId, bool up) {
  auto configFnLinkDown = [=, this](const std::shared_ptr<SwitchState>& state) {
    auto newState = state->clone();
    auto ports = newState->getPorts()->modify(&newState);
    auto port = ports->getNodeIf(portId)->clone();
    port->setAdminState(
        up ? cfg::PortState::ENABLED : cfg::PortState::DISABLED);
    ports->updateNode(port, sw()->getScopeResolver()->scope(port));
    return newState;
  };
  getSw()->updateStateBlocking("set port state", configFnLinkDown);
}

void AgentEnsembleTest::setPortLoopbackMode(
    PortID portId,
    cfg::PortLoopbackMode mode) {
  auto setLbMode = [=, this](const std::shared_ptr<SwitchState>& state) {
    auto newState = state->clone();
    auto ports = newState->getPorts()->modify(&newState);
    auto port = ports->getNodeIf(portId)->clone();
    port->setLoopbackMode(mode);
    return newState;
  };
  getSw()->updateStateBlocking("set port loopback mode", setLbMode);
}

// Returns the port names for a given list of portIDs
std::vector<std::string> AgentEnsembleTest::getPortNames(
    const std::vector<PortID>& ports) const {
  return folly::gen::from(ports) | folly::gen::map([&](PortID port) {
           return getProgrammedState()->getPorts()->getNodeIf(port)->getName();
         }) |
      folly::gen::as<std::vector<std::string>>();
}

// Waits till the link status of the passed in ports reaches
// the expected state
void AgentEnsembleTest::waitForLinkStatus(
    const std::vector<PortID>& portsToCheck,
    bool up,
    uint32_t retries,
    std::chrono::duration<uint32_t, std::milli> msBetweenRetry) const {
  XLOG(DBG2) << "Checking link status on "
             << folly::join(",", getPortNames(portsToCheck));
  auto portStatus = getSw()->getPortStatus();
  std::vector<PortID> badPorts;
  while (retries--) {
    badPorts.clear();
    for (const auto& port : portsToCheck) {
      if (*portStatus[port].up() != up) {
        std::this_thread::sleep_for(msBetweenRetry);
        portStatus = getSw()->getPortStatus();
        badPorts.push_back(port);
      }
    }
    if (badPorts.empty()) {
      return;
    }
  }

  auto msg = folly::format(
      "Unexpected Link status {:d} for {:s}",
      !up,
      folly::join(",", getPortNames(badPorts)));
  logLinkDbgMessage(badPorts);
  throw FbossError(msg);
}

void AgentEnsembleTest::assertNoInDiscards(int maxNumDiscards) {
  // When port stat is not updated yet post warmboot (collect timestamp will be
  // -1), retry another round on all ports.
  int numRounds = 0;
  auto lastStatRefTime = hardware_stats_constants::STAT_UNINITIALIZED();

  // TODO(Elangovan) Verify if the default retries are enough
  WITH_RETRIES({
    bool retry = false;
    auto portStats = getSw()->getHwPortStats(ports);
    for (auto [portID, stats] : portStats) {
      auto inDiscards = *stats.inDiscards_();
      auto port = getProgrammedState()->getPorts()->getNodeIf(port)->getName();
      XLOG(DBG2) << "Port: " << port << " in discards: " << inDiscards
                 << " in bytes: " << *stats.inBytes_()
                 << " out bytes: " << *stats.outBytes_() << " at timestamp "
                 << *stats.timestamp_();
      // Ensure that we collect 2 rounds of stats with unique timestamps
      if (*stats.timestamp_() == lastStatRefTime) {
        retry = true;
        break;
      } else {
        EXPECT_EVENTUALLY_LE(inDiscards, maxNumDiscards);
        lastStatRefTime = *stats.timestamp_();
      }
    }
    numRounds = retry ? numRounds : numRounds + 1;
    // Need at least two rounds of stats collection.
    EXPECT_EVENTUALLY_EQ(numRounds, 2);
  });
}

void AgentEnsembleTest::dumpRunningConfig(const std::string& targetDir) {
  auto testConfig = getSw()->getAgentConfig();
  auto newcfg = AgentConfig(
      testConfig,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(testConfig));
  newcfg.dumpConfig(targetDir);
}

void AgentEnsembleTest::setupConfigFile(
    const cfg::SwitchConfig& cfg,
    const std::string& testConfigDir,
    const std::string& configFile) const {
  // TODO(Elangovan) evaluate if this is needed with the move to Agent ensemble
}

void AgentEnsembleTest::reloadConfig(std::string reason) const {
  // reload config so that test config is loaded
  getSw()->applyConfig(reason, true);
}

SwitchID AgentEnsembleTest::scope(
    const boost::container::flat_set<PortDescriptor>& ports) {
  return scope(getSw()->getState(), ports);
}

SwitchID AgentEnsembleTest::scope(
    const std::shared_ptr<SwitchState>& state,
    const boost::container::flat_set<PortDescriptor>& ports) {
  auto matcher = getSw()->getScopeResolver()->scope(state, ports);
  return matcher.switchId();
}

PortID AgentEnsembleTest::getPortID(const std::string& portName) const {
  for (auto portMap : std::as_const(*getSw()->getState()->getPorts())) {
    for (auto port : std::as_const(*portMap.second)) {
      if (port.second->getName() == portName) {
        return port.second->getID();
      }
    }
  }
  throw FbossError("No port named: ", portName);
}

void AgentEnsembleTest::disableTTLDecrementOnPorts(
    const boost::container::flat_set<PortDescriptor>& ecmpPorts) {
  auto asicTable = getSw()->getHwAsicTable();
  getSw()->updateStateBlocking(
      "Disable TTL Decrement On Ports",
      [ecmpPorts, asicTable](const std::shared_ptr<SwitchState>& state) {
        auto newState = state->clone();
        for (auto port : ecmpPorts) {
          newState = utility::disableTTLDecrement(asicTable, newState, port);
        }
        return newState;
      });
}

void initAgentEnsembleTest(
    int argc,
    char** argv,
    PlatformInitFn initPlatformFn,
    std::optional<cfg::StreamType> streamType) {
  argCount = argc;
  argVec = argv;
  initEnsemble(initPlatformFn, streamType);
}

} // namespace facebook::fboss