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
using namespace std;
using namespace ns3;
using namespace ns3::mmwave; // Bring mmWave namespace into scope

// Define slice types
enum SliceType { eMBB = 0, URLLC = 1, mMTC = 2 };
NS_LOG_COMPONENT_DEFINE("Realistic5GSlicingRL");

// Global simulation parameters
uint32_t g_numUe = 10;         // Number of UEs
uint32_t g_numGnb = 2;         // Number of gNBs (base stations)
double simulationTime = 20.0; // 5 minutes simulation
double sdnInterval = 1.0;      // SDN update interval in seconds

// Global vector for UE slice assignment
std::vector<int> ueSliceAssignment;

// C++ random engine (seeded with std::random_device)
std::default_random_engine generator(std::random_device{}());

// CSV file stream for RL logging
std::ofstream csvFile;

// Packet counters (for TX and RX) per UE
std::vector<uint64_t> txPackets(g_numUe, 0);
std::vector<uint64_t> rxPackets(g_numUe, 0);

// Normalize value between min and max to [0,1]
double Normalize(double value, double minVal, double maxVal)
{
    if (maxVal - minVal == 0) return 0.0;
    return (value - minVal) / (maxVal - minVal);
}

// Compute QoS reward based on throughput (Mbps), latency (ms), and packet loss
double ComputeQoSReward(double throughput, double latency, double packetLoss, int slice)
{
    double maxThroughput, maxLatency, maxLoss;
    if (slice == eMBB) {
        maxThroughput = 150.0; maxLatency = 100.0; maxLoss = 0.1;
    } else if (slice == URLLC) {
        maxThroughput = 60.0; maxLatency = 15.0; maxLoss = 0.01;
    } else { // mMTC
        maxThroughput = 15.0; maxLatency = 500.0; maxLoss = 0.2;
    }
    double normThroughput = Normalize(throughput, 0, maxThroughput);
    double normLatency = 1.0 - Normalize(latency, 0, maxLatency);
    double normLoss = 1.0 - Normalize(packetLoss, 0, maxLoss);
    double reward = 0.5 * normThroughput - 0.3 * normLatency - 0.2 * normLoss;
    return reward;
}

// Trace callback for transmitted packets
void PacketTxTrace(uint32_t ueId, Ptr<const Packet> packet)
{
    if (ueId < g_numUe)
        txPackets[ueId]++;
}

// Trace callback for received packets
void PacketRxTrace(uint32_t ueId, Ptr<const Packet> packet)
{
    if (ueId < g_numUe)
        rxPackets[ueId]++;
}

// SDN controller callback to simulate performance, decide on slice reassignments, and log RL data
void SDNControllerCallback(NodeContainer ueNodes, NetDeviceContainer gnbDevices)
{
    for (uint32_t i = 0; i < ueNodes.GetN(); i++)
    {
        int currSlice = ueSliceAssignment[i];
        double throughput, latency, packetLoss;
        std::uniform_real_distribution<double> thrDist(
            (currSlice == eMBB) ? 20 : (currSlice == URLLC) ? 10 : 0.5,
            (currSlice == eMBB) ? 150 : (currSlice == URLLC) ? 60 : 15);
        throughput = thrDist(generator);
        std::uniform_real_distribution<double> latDist(
            (currSlice == eMBB) ? 20 : (currSlice == URLLC) ? 1 : 50,
            (currSlice == eMBB) ? 100 : (currSlice == URLLC) ? 15 : 500);
        latency = latDist(generator);
        uint64_t tx = txPackets[i];
        uint64_t rx = rxPackets[i];
        packetLoss = (tx > 0) ? 1.0 - static_cast<double>(rx)/tx : 0.0;
        double reward = ComputeQoSReward(throughput, latency, packetLoss, currSlice);
        int action = 0;
        if (reward < 0.5) {
            std::uniform_int_distribution<int> sliceDist(0, 2);
            int newSlice = sliceDist(generator);
            while(newSlice == currSlice)
                newSlice = sliceDist(generator);
            action = 1;
            ueSliceAssignment[i] = newSlice;
        }
        csvFile << Simulator::Now().GetSeconds() << "," << i << "," << currSlice << "," << action << ","
                << throughput << "," << latency << "," << packetLoss << "," << reward << "\n";
    }
    Simulator::Schedule(Seconds(sdnInterval), &SDNControllerCallback, ueNodes, gnbDevices);
}

// Custom application for dynamic traffic generation with burst behavior
class DynamicTrafficApp : public Application
{
public:
    DynamicTrafficApp() : m_socket(0), m_running(false), m_ueId(0), m_burstSize(0), m_packetsInBurst(0) {}
    void Setup(Address address, uint32_t ueId) { m_peer = address; m_ueId = ueId; }
private:
    virtual void StartApplication(void)
    {
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind();
        m_socket->Connect(m_peer);
        m_socket->TraceConnectWithoutContext("Tx", MakeBoundCallback(&PacketTxTrace, m_ueId));
        ScheduleNextBurst();
    }
    virtual void StopApplication(void)
    {
        m_running = false;
        Simulator::Cancel(m_sendEvent);
        if (m_socket) m_socket->Close();
    }
    void ScheduleNextBurst()
    {
        int slice = ueSliceAssignment[m_ueId];
        std::gamma_distribution<double> burstInterval(
            (slice == eMBB) ? 2.0 : (slice == URLLC) ? 3.0 : 1.5,
            (slice == eMBB) ? 0.1 : (slice == URLLC) ? 0.02 : 0.5);
        double interval = burstInterval(generator);
        std::uniform_int_distribution<int> burstSizeDist(
            (slice == eMBB) ? 5 : (slice == URLLC) ? 1 : 10,
            (slice == eMBB) ? 14 : (slice == URLLC) ? 5 : 29);
        m_burstSize = burstSizeDist(generator);
        m_packetsInBurst = 0;
        m_sendEvent = Simulator::Schedule(Seconds(interval), &DynamicTrafficApp::SendBurst, this);
    }
    void SendBurst()
    {
        int slice = ueSliceAssignment[m_ueId];
        std::uniform_int_distribution<uint32_t> sizeDist(
            (slice == eMBB) ? 500 : (slice == URLLC) ? 100 : 50,
            (slice == eMBB) ? 1500 : (slice == URLLC) ? 600 : 300);
        std::uniform_real_distribution<double> intraBurstDist(0.001,
            (slice == eMBB) ? 0.05 : (slice == URLLC) ? 0.01 : 0.1);
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

int main (int argc, char *argv[])
{
    LogComponentEnable("Realistic5GSlicingRL", LOG_LEVEL_INFO);
    csvFile.open("rl_dataset.csv");
    csvFile << "Time,UE_ID,CurrentSlice,Action,Throughput(Mbps),Latency(ms),PacketLoss,Reward\n";

    // Create nodes: UEs, gNBs, and one VNF node
    NodeContainer ueNodes, gnbNodes, vnfNodes;
    ueNodes.Create(g_numUe);
    gnbNodes.Create(g_numGnb);
    vnfNodes.Create(1);

    // Set mobility models
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(gnbNodes); // gNBs are stationary
    mobility.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                              "Bounds", RectangleValue(Rectangle(-2500, 2500, -2500, 2500)));
    mobility.Install(ueNodes);  // UEs are mobile
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(vnfNodes); // VNF is stationary

    // Install building information for realistic propagation
    BuildingsHelper::Install(gnbNodes);
    BuildingsHelper::Install(ueNodes);
    BuildingsHelper::Install(vnfNodes);

    // Install internet stack on all nodes
    InternetStackHelper internet;
    internet.Install(gnbNodes);
    internet.Install(ueNodes);
    internet.Install(vnfNodes);

    // Set up mmWave network using Buildings channel condition model
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

    // Set up point-to-point link between one gNB and the VNF node
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("500Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue(QueueSize("500p")));
    NetDeviceContainer p2pDevices = p2p.Install(gnbNodes.Get(0), vnfNodes.Get(0));

    // Assign IP addresses to the P2P link on a separate subnet
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer p2pIfaces = ipv4.Assign(p2pDevices);
    Ipv4Address vnfIp = p2pIfaces.GetAddress(1); // VNF's IP

    // Initialize UE slice assignments using C++ random engine
    ueSliceAssignment.resize(g_numUe);
    std::uniform_int_distribution<int> sliceDist(0, 2);
    for (uint32_t i = 0; i < g_numUe; i++) {
        ueSliceAssignment[i] = sliceDist(generator);
    }

    // Install dynamic traffic applications on each UE
    uint16_t destPort = 4000;
    for (uint32_t i = 0; i < ueNodes.GetN(); i++) {
        Ptr<DynamicTrafficApp> app = CreateObject<DynamicTrafficApp>();
        app->Setup(InetSocketAddress(vnfIp, destPort), i);
        ueNodes.Get(i)->AddApplication(app);
        std::uniform_real_distribution<double> startTime(0.1, 10.0);
        app->SetStartTime(Seconds(startTime(generator)));
        app->SetStopTime(Seconds(simulationTime));
        Ptr<NetDevice> gnbDev = gnbDevices.Get(i % gnbDevices.GetN());
        if (gnbDev) {
            gnbDev->TraceConnectWithoutContext("MacRx", MakeBoundCallback(&PacketRxTrace, i));
        }
    }

    // Schedule SDN controller updates to adjust slices and log RL data
    Simulator::Schedule(Seconds(sdnInterval), &SDNControllerCallback, ueNodes, gnbDevices);

    // Set up NetAnim for visualization
    AnimationInterface anim("realistic_5g_slicing_rl_anim.xml");
    anim.SetMaxPktsPerTraceFile(500000);
    anim.EnablePacketMetadata(true); // Optional: Adds packet flow arrows for more "reality"
for (uint32_t i = 0; i < ueNodes.GetN(); i++) {
    anim.UpdateNodeDescription(ueNodes.Get(i), "UE_" + std::to_string(ueSliceAssignment[i]));
    switch (ueSliceAssignment[i]) {
        case eMBB:  anim.UpdateNodeColor(ueNodes.Get(i), 0, 0, 255); break;   // Blue
        case URLLC: anim.UpdateNodeColor(ueNodes.Get(i), 255, 255, 0); break; // Yellow
        case mMTC:  anim.UpdateNodeColor(ueNodes.Get(i), 0, 255, 255); break; // Cyan
    }
    anim.UpdateNodeSize(ueNodes.Get(i)->GetId(), 10, 10);
}
for (uint32_t i = 0; i < gnbNodes.GetN(); i++) {
    anim.UpdateNodeDescription(gnbNodes.Get(i), "gNB");
    anim.UpdateNodeColor(gnbNodes.Get(i), 0, 255, 0); // Green
    anim.UpdateNodeSize(gnbNodes.Get(i)->GetId(), 20, 20);
}

    Simulator::Stop(Seconds(simulationTime));
    Simulator::Run();
    Simulator::Destroy();
    csvFile.close();
    return 0;
}
