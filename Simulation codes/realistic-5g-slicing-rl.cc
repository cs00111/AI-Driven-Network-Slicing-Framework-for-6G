#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/mmwave-module.h"
#include "ns3/buildings-module.h"
#include "ns3/netanim-module.h"
#include <fstream>
#include <random>
#include <vector>

using namespace ns3;
using namespace ns3::mmwave; // Add this to bring mmwave namespace into scope

// Define slice types
enum SliceType { eMBB = 0, URLLC = 1, mMTC = 2 };
NS_LOG_COMPONENT_DEFINE("Realistic5GSlicingRL");

// Global simulation parameters
uint32_t g_numUe = 10;         // Number of User Equipments (UEs)
uint32_t g_numGnb = 2;         // Number of gNodeBs (gNBs)
double simulationTime = 300.0; // Simulation time in seconds (5 minutes)
double sdnInterval = 1.0;      // SDN controller update interval in seconds
std::vector<int> ueSliceAssignment; // Slice assignment for each UE
std::default_random_engine generator(std::random_device{}());
std::ofstream csvFile; // Output file for logging

// Packet counters for tracing
std::vector<uint64_t> txPackets(g_numUe, 0); // Transmitted packets per UE
std::vector<uint64_t> rxPackets(g_numUe, 0); // Received packets per UE

// Normalize values between 0 and 1
double Normalize(double value, double minVal, double maxVal)
{
    if (maxVal - minVal == 0) return 0.0;
    return (value - minVal) / (maxVal - minVal);
}

// Compute QoS reward based on slice type
double ComputeQoSReward(double throughput, double latency, double packetLoss, int slice)
{
    double maxThroughput, maxLatency, maxLoss;
    if (slice == eMBB) {
        maxThroughput = 150.0; maxLatency = 100.0; maxLoss = 0.1; // eMBB: High throughput
    } else if (slice == URLLC) {
        maxThroughput = 60.0; maxLatency = 15.0; maxLoss = 0.01;  // URLLC: Low latency
    } else { // mMTC
        maxThroughput = 15.0; maxLatency = 500.0; maxLoss = 0.2;  // mMTC: High connection density
    }
    double normThroughput = Normalize(throughput, 0, maxThroughput);
    double normLatency = 1.0 - Normalize(latency, 0, maxLatency);
    double normLoss = 1.0 - Normalize(packetLoss, 0, maxLoss);
    double reward = 0.5 * normThroughput + 0.3 * normLatency + 0.2 * normLoss;
    return std::max(0.0, std::min(1.0, reward));
}

// Trace callback for transmitted packets
void PacketTxTrace(uint32_t ueId, Ptr<const Packet> packet)
{
    if (ueId < g_numUe) txPackets[ueId]++;
}

// Trace callback for received packets
void PacketRxTrace(uint32_t ueId, Ptr<const Packet> packet)
{
    if (ueId < g_numUe) rxPackets[ueId]++;
}

// SDN controller logic to adjust slice assignments
void SDNControllerCallback(NodeContainer ueNodes, NetDeviceContainer gnbDevices)
{
    for (uint32_t i = 0; i < ueNodes.GetN(); i++)
    {
        int currSlice = ueSliceAssignment[i];
        double throughput, latency, packetLoss;

        // Simulate throughput (Mbps) based on slice type
        std::uniform_real_distribution<double> thrDist(currSlice == eMBB ? 20 : currSlice == URLLC ? 10 : 0.5,
                                                      currSlice == eMBB ? 150 : currSlice == URLLC ? 60 : 15);
        throughput = thrDist(generator);

        // Simulate latency (ms) based on slice type
        std::uniform_real_distribution<double> latDist(currSlice == eMBB ? 20 : currSlice == URLLC ? 1 : 50,
                                                      currSlice == eMBB ? 100 : currSlice == URLLC ? 15 : 500);
        latency = latDist(generator);

        // Calculate packet loss from traced packet counts
        uint64_t tx = txPackets[i];
        uint64_t rx = rxPackets[i];
        packetLoss = (tx > 0) ? 1.0 - static_cast<double>(rx) / tx : 0.0;

        double reward = ComputeQoSReward(throughput, latency, packetLoss, currSlice);
        int action = 0; // 0: No change, 1: Slice reassigned
        if (reward < 0.5) { // Reassign slice if reward is low
            int newSlice = rand() % 3;
            while (newSlice == currSlice) newSlice = rand() % 3;
            action = 1;
            ueSliceAssignment[i] = newSlice;
        }

        // Log metrics to CSV file
        csvFile << Simulator::Now().GetSeconds() << "," << i << "," << currSlice << "," << action << ","
                << throughput << "," << latency << "," << packetLoss << "," << reward << "\n";
    }
    Simulator::Schedule(Seconds(sdnInterval), &SDNControllerCallback, ueNodes, gnbDevices);
}

// Custom application for dynamic traffic generation
class DynamicTrafficApp : public Application
{
public:
    DynamicTrafficApp() : m_socket(0), m_running(false), m_ueId(0), m_burstSize(0), m_packetsInBurst(0) {}
    void Setup(Address address, uint32_t ueId) { m_peer = address; m_ueId = ueId; }
private:
    void StartApplication() override
    {
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind();
        m_socket->Connect(m_peer);
        m_socket->TraceConnectWithoutContext("Tx", MakeBoundCallback(&PacketTxTrace, m_ueId));
        ScheduleNextBurst();
    }
    void StopApplication() override
    {
        m_running = false;
        Simulator::Cancel(m_sendEvent);
        if (m_socket) m_socket->Close();
    }
    void ScheduleNextBurst()
    {
        int slice = ueSliceAssignment[m_ueId];
        std::gamma_distribution<double> burstInterval(slice == eMBB ? 2.0 : slice == URLLC ? 3.0 : 1.5,
                                                     slice == eMBB ? 0.1 : slice == URLLC ? 0.02 : 0.5);
        double interval = burstInterval(generator);
        m_burstSize = (slice == eMBB ? rand() % 10 + 5 : slice == URLLC ? rand() % 5 + 1 : rand() % 20 + 10);
        m_packetsInBurst = 0;
        m_sendEvent = Simulator::Schedule(Seconds(interval), &DynamicTrafficApp::SendBurst, this);
    }
    void SendBurst()
    {
        int slice = ueSliceAssignment[m_ueId];
        std::uniform_int_distribution<uint32_t> sizeDist(slice == eMBB ? 500 : slice == URLLC ? 100 : 50,
                                                        slice == eMBB ? 1500 : slice == URLLC ? 600 : 300);
        std::uniform_real_distribution<double> intraBurstDist(0.001, slice == eMBB ? 0.05 : slice == URLLC ? 0.01 : 0.1);
        
        if (m_packetsInBurst < m_burstSize) {
            Ptr<Packet> packet = Create<Packet>(sizeDist(generator));
            if (m_socket) m_socket->Send(packet);
            m_packetsInBurst++;
            Simulator::Schedule(Seconds(intraBurstDist(generator)), &DynamicTrafficApp::SendBurst, this);
        } else if (m_running) {
            ScheduleNextBurst();
        }
    }

    Ptr<Socket> m_socket;
    Address m_peer;
    EventId m_sendEvent;
    bool m_running;
    uint32_t m_ueId;
    uint32_t m_burstSize;
    uint32_t m_packetsInBurst;
};

int main(int argc, char *argv[])
{
    LogComponentEnable("Realistic5GSlicingRL", LOG_LEVEL_INFO);

    // Open CSV file for logging
    csvFile.open("rl_dataset.csv");
    csvFile << "Time,UE_ID,CurrentSlice,Action,Throughput(Mbps),Latency(ms),PacketLoss,Reward\n";

    // Create nodes
    NodeContainer ueNodes, gnbNodes, vnfNodes;
    ueNodes.Create(g_numUe);    // User Equipments
    gnbNodes.Create(g_numGnb);  // gNodeBs (base stations)
    vnfNodes.Create(1);         // Virtual Network Function node

    // Install mobility models (before BuildingsHelper)
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(gnbNodes);
    mobility.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                              "Bounds", RectangleValue(Rectangle(-2500, 2500, -2500, 2500)));
    mobility.Install(ueNodes);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(vnfNodes);

    // Install building-related info
    BuildingsHelper::Install(gnbNodes);
    BuildingsHelper::Install(ueNodes);
    BuildingsHelper::Install(vnfNodes);

    // Install internet stack
    InternetStackHelper internet;
    internet.Install(gnbNodes);
    internet.Install(ueNodes);
    internet.Install(vnfNodes);

    // Set up mmWave network
    Ptr<MmWaveHelper> mmwaveHelper = CreateObject<MmWaveHelper>();
    mmwaveHelper->SetChannelConditionModelType("ns3::BuildingsChannelConditionModel");
    NetDeviceContainer gnbDevices = mmwaveHelper->InstallEnbDevice(gnbNodes);
    NetDeviceContainer ueDevices = mmwaveHelper->InstallUeDevice(ueNodes);

    // Assign IP addresses to mmWave devices
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ueIfaces = ipv4.Assign(ueDevices);
    Ipv4InterfaceContainer gnbIfaces = ipv4.Assign(gnbDevices);

    // Attach UEs to the closest gNB
    mmwaveHelper->AttachToClosestEnb(ueDevices, gnbDevices);

    // Set up point-to-point link between gNB and VNF
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("500Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue(QueueSize("500p")));
    NetDeviceContainer p2pDevices = p2p.Install(gnbNodes.Get(0), vnfNodes.Get(0));

    // Assign IP addresses to P2P devices
    ipv4.SetBase("10.1.2.0", "255.255.255.0"); // Separate subnet for P2P link
    Ipv4InterfaceContainer p2pIfaces = ipv4.Assign(p2pDevices);

    // Initialize slice assignments
    ueSliceAssignment.resize(g_numUe);
    for (uint32_t i = 0; i < g_numUe; i++) {
        ueSliceAssignment[i] = rand() % 3; // eMBB, URLLC, or mMTC
    }

    // Install applications and traces
    uint16_t destPort = 4000;
    Ipv4Address vnfIp = p2pIfaces.GetAddress(1); // VNF’s P2P interface
    for (uint32_t i = 0; i < ueNodes.GetN(); i++) {
        Ptr<DynamicTrafficApp> app = CreateObject<DynamicTrafficApp>();
        app->Setup(InetSocketAddress(vnfIp, destPort), i);
        ueNodes.Get(i)->AddApplication(app);
        std::uniform_real_distribution<double> startTime(0.1, 10.0);
        app->SetStartTime(Seconds(startTime(generator)));
        app->SetStopTime(Seconds(simulationTime));

        // Connect Rx trace at gNB
        Ptr<NetDevice> gnbDev = gnbDevices.Get(i % gnbDevices.GetN());
        if (gnbDev) {
            gnbDev->TraceConnectWithoutContext("MacRx", MakeBoundCallback(&PacketRxTrace, i));
        }
    }

    // Schedule SDN controller updates
    Simulator::Schedule(Seconds(sdnInterval), &SDNControllerCallback, ueNodes, gnbDevices);

    // Set up animation
    AnimationInterface anim("realistic_5g_slicing_rl_anim.xml");
    anim.SetMaxPktsPerTraceFile(500000);

    // Run simulation
    Simulator::Stop(Seconds(simulationTime));
    Simulator::Run();
    Simulator::Destroy();
    csvFile.close();
    return 0;
}
