#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/mmwave-module.h"        // ns3-mmWave module header
#include "ns3/netanim-module.h"
#include <fstream>
#include <random>
#include <vector>
using namespace ns3;

// Define slice types
enum SliceType { eMBB = 0, URLLC = 1, mMTC = 2 };

NS_LOG_COMPONENT_DEFINE("Advanced5GSlicingRL");

// Global simulation parameters
uint32_t g_numUe = 10;
uint32_t g_numGnb = 2;
double simulationTime = 20.0; // seconds
double sdnInterval = 1.0;     // SDN decision interval (seconds)

// Global vector storing current slice assignment for each UE
std::vector<int> ueSliceAssignment;

// Global random number generator
std::default_random_engine generator;

// CSV file stream for RL dataset logging
std::ofstream csvFile;

// Utility: Normalize value to [0,1]
double Normalize(double value, double minVal, double maxVal)
{
    if (maxVal - minVal == 0) return 0.0;
    return (value - minVal) / (maxVal - minVal);
}

// Compute QoS reward given throughput (Mbps) and latency (ms) for a given slice
double ComputeQoSReward(double throughput, double latency, int slice)
{
    double desiredThroughput, desiredLatency;
    if (slice == eMBB) {
        desiredThroughput = 80.0;  // high throughput desired
        desiredLatency = 50.0;     // moderate latency acceptable
    } else if (slice == URLLC) {
        desiredThroughput = 30.0;  // moderate throughput
        desiredLatency = 5.0;      // ultra-low latency desired
    } else { // mMTC
        desiredThroughput = 5.0;   // low throughput needed
        desiredLatency = 200.0;    // higher latency acceptable
    }
    double normThroughput = Normalize(throughput, 0, desiredThroughput * 1.5);
    double normLatency = 1.0 - Normalize(latency, 0, desiredLatency * 2);
    double reward = 0.6 * normThroughput + 0.4 * normLatency;
    if (reward < 0) reward = 0;
    if (reward > 1) reward = 1;
    return reward;
}

// SDN Controller Callback: Simulate decisions by checking each UE's performance,
// possibly reassigning slices, and logging state-action-reward to CSV.
void SDNControllerCallback(NodeContainer ueNodes)
{
    for (uint32_t i = 0; i < ueNodes.GetN(); i++)
    {
        int currSlice = ueSliceAssignment[i];
        double throughput, latency;
        // Simulate throughput and latency using randomized values per slice.
        if (currSlice == eMBB) {
            std::uniform_real_distribution<double> thrDist(40.0, 100.0);
            throughput = thrDist(generator);
            std::uniform_real_distribution<double> latDist(30.0, 70.0);
            latency = latDist(generator);
        }
        else if (currSlice == URLLC) {
            std::uniform_real_distribution<double> thrDist(20.0, 50.0);
            throughput = thrDist(generator);
            std::uniform_real_distribution<double> latDist(2.0, 8.0);
            latency = latDist(generator);
        }
        else { // mMTC
            std::uniform_real_distribution<double> thrDist(1.0, 10.0);
            throughput = thrDist(generator);
            std::uniform_real_distribution<double> latDist(100.0, 300.0);
            latency = latDist(generator);
        }
        double reward = ComputeQoSReward(throughput, latency, currSlice);
        int action = 0; // 0: no change, 1: switch slice
        if (reward < 0.5)
        {
            int newSlice = currSlice;
            while (newSlice == currSlice)
            {
                newSlice = generator() % 3;
            }
            action = 1;
            ueSliceAssignment[i] = newSlice;
        }
        // Log: Time, UE_ID, CurrentSlice, Action, Throughput, Latency, Reward
        csvFile << Simulator::Now().GetSeconds() << "," << i << ","
                << currSlice << "," << action << "," << throughput << ","
                << latency << "," << reward << "\n";
    }
    Simulator::Schedule(Seconds(sdnInterval), &SDNControllerCallback, ueNodes);
}

// --- Custom Application: DynamicTrafficApp ---
// Sends UDP packets with dynamic parameters: exponential distribution for inter-packet intervals
// (simulating a Poisson process) and normal distributions for packet sizes. Parameters vary with the current slice.
class DynamicTrafficApp : public Application
{
public:
  DynamicTrafficApp() : m_socket(0), m_running(false), m_ueId(0) {}
  virtual ~DynamicTrafficApp() { m_socket = 0; }
  void Setup(Address address, uint32_t ueId) {
    m_peer = address;
    m_ueId = ueId;
  }
private:
  virtual void StartApplication(void);
  virtual void StopApplication(void);
  void SendPacket(void);
  Ptr<Socket> m_socket;
  Address m_peer;
  EventId m_sendEvent;
  bool m_running;
  uint32_t m_ueId; // Index corresponding to ueSliceAssignment
};

void DynamicTrafficApp::StartApplication(void)
{
  m_running = true;
  if (!m_socket)
  {
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_socket->Connect(m_peer);
  }
  SendPacket();
}

void DynamicTrafficApp::StopApplication(void)
{
  m_running = false;
  if (m_sendEvent.IsPending())
    Simulator::Cancel(m_sendEvent);
  if (m_socket)
    m_socket->Close();
}

void DynamicTrafficApp::SendPacket(void)
{
  int currentSlice = ueSliceAssignment[m_ueId];
  uint32_t packetSize = 0;
  double interval = 0.0;
  
  if (currentSlice == eMBB) {
      std::normal_distribution<double> sizeDist(1000, 100);
      packetSize = std::max(100u, static_cast<unsigned int>(sizeDist(generator)));
      std::exponential_distribution<double> expDist(20.0);
      interval = expDist(generator);
  } else if (currentSlice == URLLC) {
      std::normal_distribution<double> sizeDist(400, 50);
      packetSize = std::max(50u, static_cast<unsigned int>(sizeDist(generator)));
      std::exponential_distribution<double> expDist(100.0);
      interval = expDist(generator);
  } else { // mMTC
      std::normal_distribution<double> sizeDist(150, 30);
      packetSize = std::max(50u, static_cast<unsigned int>(sizeDist(generator)));
      std::exponential_distribution<double> expDist(2.0);
      interval = expDist(generator);
  }
  
  Ptr<Packet> packet = Create<Packet>(packetSize);
  m_socket->Send(packet);
  
  if (m_running)
    m_sendEvent = Simulator::Schedule(Seconds(interval), &DynamicTrafficApp::SendPacket, this);
}

// --- Main Simulation ---
int main (int argc, char *argv[])
{
  LogComponentEnable("Advanced5GSlicingRL", LOG_LEVEL_INFO);

  csvFile.open("rl_dataset.csv");
  csvFile << "Time,UE_ID,CurrentSlice,Action,Throughput(Mbps),Latency(ms),Reward\n";

  NodeContainer ueNodes;
  ueNodes.Create(g_numUe);
  NodeContainer gnbNodes;
  gnbNodes.Create(g_numGnb);
  NodeContainer vnfNodes;
  vnfNodes.Create(1);

  MobilityHelper mobility;
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(gnbNodes);
  
  MobilityHelper mobilityUe;
  mobilityUe.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                              "Bounds", RectangleValue(Rectangle(-2500, 2500, -2500, 2500)));
  mobilityUe.Install(ueNodes);
  
  MobilityHelper mobilityVnf;
  mobilityVnf.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobilityVnf.Install(vnfNodes);

  InternetStackHelper internet;
  internet.Install(ueNodes);
  internet.Install(gnbNodes);
  internet.Install(vnfNodes);

  Ptr<mmwave::MmWaveHelper> mmwaveHelper = CreateObject<mmwave::MmWaveHelper>();

  NetDeviceContainer gnbDevices = mmwaveHelper->InstallEnbDevice(gnbNodes);
  NetDeviceContainer ueDevices = mmwaveHelper->InstallUeDevice(ueNodes);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer ueIfaces = ipv4.Assign(ueDevices);
  Ipv4InterfaceContainer gnbIfaces = ipv4.Assign(gnbDevices);

  mmwaveHelper->AttachToClosestEnb(ueDevices, gnbDevices);

  // Connect each gNB individually to the VNF node using point-to-point links.
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
  p2p.SetChannelAttribute("Delay", StringValue("5ms"));
  for (uint32_t i = 0; i < gnbNodes.GetN(); i++) {
      NodeContainer pair;
      pair.Add(gnbNodes.Get(i));
      pair.Add(vnfNodes.Get(0));
      p2p.Install(pair);
  }

  ueSliceAssignment.resize(g_numUe);
  for (uint32_t i = 0; i < g_numUe; i++) {
      ueSliceAssignment[i] = generator() % 3;
  }

  Simulator::Schedule(Seconds(sdnInterval), &SDNControllerCallback, ueNodes);

  uint16_t destPort = 4000;
  Ipv4Address vnfIp = vnfNodes.Get(0)->GetObject<Ipv4>()->GetAddress(0,0).GetLocal();
  for (uint32_t i = 0; i < ueNodes.GetN(); i++) {
      Ptr<DynamicTrafficApp> app = CreateObject<DynamicTrafficApp>();
      app->Setup(InetSocketAddress(vnfIp, destPort), i);
      ueNodes.Get(i)->AddApplication(app);
      app->SetStartTime(Seconds(2.0 + i * 0.1));
      app->SetStopTime(Seconds(simulationTime));
  }

  AnimationInterface anim("advanced_5g_slicing_rl_anim.xml");
  anim.SetMaxPktsPerTraceFile(500000);

  Simulator::Stop(Seconds(simulationTime));
  Simulator::Run();
  Simulator::Destroy();

  csvFile.close();
  return 0;
}
