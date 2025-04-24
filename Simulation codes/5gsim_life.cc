#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/mmwave-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/netanim-module.h"
#include <fstream>
#include <random>
#include <vector>
#include <map>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("DynamicTrafficApp");

enum TrafficType { eMBB = 0, URLLC = 1, mMTC = 2 };
const uint32_t numUes = 40;
const uint32_t numRouters = 6;
const double simTime = 20.0;
const double sdnInterval = 0.01; // Sub-second steps for detailed logging
const double areaSize = 50.0;
const double nodeRadius = 0.3; // 3 cm display size in NetAnim
const double routerBandwidth = 1e9; // 1 Gbps
const double ueSpeedRange[2] = {2.0, 4.0}; // m/s

std::default_random_engine generator(1234);
std::ofstream csvFile;
std::vector<int> ueTrafficType; // Slice ID (0: eMBB, 1: URLLC, 2: mMTC)
int currentAction = 3; // Default: A4 (Do nothing)

class DynamicTrafficApp : public Application
{
public:
    DynamicTrafficApp() : m_socket(0), m_running(false), m_ueId(0), m_lastPacketTime(0.0), m_queueSize(0) {}
    virtual ~DynamicTrafficApp() { m_socket = 0; }
    void Setup(Address address, uint32_t ueId)
    {
        m_peer = address;
        m_ueId = ueId;
    }
    double GetLastPacketTime() const { return m_lastPacketTime; }
    uint32_t GetQueueSize() const { return m_queueSize; }

private:
    virtual void StartApplication(void)
    {
        NS_LOG_FUNCTION(this);
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        if (!m_socket)
        {
            NS_LOG_ERROR("Failed to create socket for UE " << m_ueId);
            return;
        }
        m_socket->Connect(m_peer);
        NS_LOG_INFO("Socket connected for UE " << m_ueId << " to " << m_peer);
        SchedulePacket();
    }
    virtual void StopApplication(void)
    {
        NS_LOG_FUNCTION(this);
        m_running = false;
        if (m_sendEvent.IsPending()) Simulator::Cancel(m_sendEvent);
        if (m_socket) m_socket->Close();
    }
    void SchedulePacket(void)
    {
        NS_LOG_FUNCTION(this);
        if (!m_running) return;

        int sliceId = ueTrafficType[m_ueId];
        uint32_t packetSize = 0;
        double interval = 0.0;

        if (sliceId == eMBB)
        {
            std::exponential_distribution<double> sizeDist(1.00 / 1500.00);
            packetSize = std::max(100u, static_cast<uint32_t>(sizeDist(generator)));
            std::exponential_distribution<double> rateDist(20.0);
            interval = rateDist(generator) / 1000.0; // ~0.05-0.1s
        }
        else if (sliceId == URLLC)
        {
            std::normal_distribution<double> sizeDist(500, 100);
            packetSize = std::max(50u, static_cast<uint32_t>(sizeDist(generator)));
            std::poisson_distribution<int> rateDist(100);
            interval = rateDist(generator) / 1000.0; // ~0.01-0.02s
        }
        else // mMTC
        {
            std::normal_distribution<double> sizeDist(200, 50);
            packetSize = std::max(50u, static_cast<uint32_t>(sizeDist(generator)));
            std::exponential_distribution<double> rateDist(2.0);
            interval = rateDist(generator) / 1000.0; // ~0.5-1s
        }

        m_queueSize++;
        Ptr<Packet> packet = Create<Packet>(packetSize);
        if (m_socket->Send(packet) >= 0)
        {
            NS_LOG_INFO("UE " << m_ueId << " sent packet of size " << packetSize << " at " << Simulator::Now().GetSeconds());
            m_lastPacketTime = Simulator::Now().GetSeconds();
        }
        else
            NS_LOG_ERROR("Failed to send packet from UE " << m_ueId);

        if (m_running)
            m_sendEvent = Simulator::Schedule(Seconds(interval), &DynamicTrafficApp::SchedulePacket, this);
    }

    Ptr<Socket> m_socket;
    Address m_peer;
    EventId m_sendEvent;
    bool m_running;
    uint32_t m_ueId;
    double m_lastPacketTime;
    uint32_t m_queueSize;
};

int getState(const std::map<int, double>& trafficStats)
{
    double eMBB = trafficStats.at(0); // eMBB
    double URLLC = trafficStats.at(1); // URLLC
    double mMTC = trafficStats.at(2);  // mMTC
    if (eMBB > URLLC && eMBB > mMTC) return 0; // S1: eMBB Dominant
    else if (URLLC > eMBB && URLLC > mMTC) return 1; // S2: URLLC Dominant
    else return 2; // S3: mMTC Dominant
}

void SDNControllerCallback(Ptr<FlowMonitor> monitor, Ptr<Ipv4FlowClassifier> classifier, NodeContainer ues)
{
    NS_LOG_FUNCTION_NOARGS();
    monitor->CheckForLostPackets();
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();
    std::map<int, double> trafficStats = {{0, 0.0}, {1, 0.0}, {2, 0.0}}; // eMBB, URLLC, mMTC
    double totalThroughput = 0.0, totalLatency = 0.0, totalPacketLoss = 0.0;

    for (uint32_t i = 0; i < ues.GetN(); ++i)
    {
        int sliceId = ueTrafficType[i];
        double throughput = 0.0, latency = 0.0, packetLoss = 0.0;
        Ptr<Ipv4> ipv4 = ues.Get(i)->GetObject<Ipv4>();
        if (!ipv4) continue;
        Ipv4Address ueAddr = ipv4->GetAddress(1, 0).GetLocal();
        NS_LOG_INFO("UE " << i << " IP: " << ueAddr);

        for (auto it = stats.begin(); it != stats.end(); ++it)
        {
            Ipv4FlowClassifier::FiveTuple tuple = classifier->FindFlow(it->first);
            if (tuple.sourceAddress == ueAddr)
            {
                double duration = it->second.timeLastRxPacket.GetSeconds() - it->second.timeFirstTxPacket.GetSeconds();
                if (duration > 0)
                {
                    throughput = it->second.rxBytes * 8.0 / duration / 1e6; // Mbps
                    latency = it->second.delaySum.GetMilliSeconds() / std::max(1.0, static_cast<double>(it->second.rxPackets));
                    packetLoss = it->second.lostPackets / std::max(1.0, static_cast<double>(it->second.txPackets + it->second.lostPackets));
                }
                NS_LOG_INFO("UE " << i << " throughput: " << throughput << " Mbps");
                break;
            }
        }

        Ptr<DynamicTrafficApp> app = ues.Get(i)->GetApplication(0)->GetObject<DynamicTrafficApp>();
        uint32_t queueSize = app->GetQueueSize();
        double resourceAllocation = routerBandwidth / numRouters / 1e6; // Mbps per UE
        double reward = (throughput / 50.0) - (latency / 100.0) + (queueSize < 5 ? 1.0 : -1.0); // Simplified reward

        trafficStats[sliceId] += throughput;
        totalThroughput += throughput;
        totalLatency += latency;
        totalPacketLoss += packetLoss;

        csvFile << Simulator::Now().GetSeconds() << "," << sliceId << "," << i << ","
                << app->GetLastPacketTime() << "," << (queueSize * 1500) << "," // Approx packet size
                << throughput << "," << latency << "," << queueSize << ","
                << resourceAllocation << "," << reward << "\n";
    }

    int state = getState(trafficStats);
    int nextAction = currentAction;
    double reward = 0.0;
    if (state == 0) // S1: eMBB Dominant
    {
        if (currentAction == 0) reward = -5;
        else if (currentAction == 1) reward = 5;
        else if (currentAction == 2) reward = 0;
        else reward = 0;
        nextAction = (Simulator::Now().GetSeconds() > 10) ? 1 : currentAction;
    }
    else if (state == 1) // S2: URLLC Dominant
    {
        if (currentAction == 0) reward = 10;
        else if (currentAction == 1) reward = 5;
        else if (currentAction == 2) reward = 0;
        else reward = -10;
        nextAction = (Simulator::Now().GetSeconds() > 15) ? 0 : currentAction;
    }
    else // S3: mMTC Dominant
    {
        if (currentAction == 0) reward = -10;
        else if (currentAction == 1) reward = 5;
        else if (currentAction == 2) reward = 10;
        else reward = -5;
        nextAction = (Simulator::Now().GetSeconds() > 20) ? 2 : currentAction;
    }

    if (nextAction != currentAction)
    {
        for (uint32_t i = 0; i < numUes; ++i)
        {
            if (nextAction == 0 && ueTrafficType[i] != 1) ueTrafficType[i] = 1;
            else if (nextAction == 2 && ueTrafficType[i] == 2)
                ueTrafficType[i] = std::uniform_int_distribution<int>(0, 1)(generator);
        }
        currentAction = nextAction;
    }

    csvFile << Simulator::Now().GetSeconds() << ",STATE,-1,-1,-1,-1,-1,-1,-1," << reward << "\n";

    Simulator::Schedule(Seconds(sdnInterval), &SDNControllerCallback, monitor, classifier, ues);
}

void PacketSinkReceive(Ptr<Socket> socket)
{
    Ptr<Packet> packet;
    while ((packet = socket->Recv()))
    {
        NS_LOG_INFO("Received packet of size " << packet->GetSize() << " at port " << socket->GetBoundNetDevice()->GetIfIndex());
    }
}

int main(int argc, char *argv[])
{
    LogComponentEnable("DynamicTrafficApp", LOG_LEVEL_INFO);

    csvFile.open("life.csv");
    csvFile << "Time Step,Slice ID,UE ID,Packet Arrival Time (s),Packet Size (bytes),Throughput (Mbps),Latency (ms),Queue Size,Resource Allocation (Mbps),Reward\n";

    NodeContainer ues, routers, servers;
    ues.Create(numUes);
    routers.Create(numRouters);
    servers.Create(1);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                             "Bounds", RectangleValue(Rectangle(0, areaSize, 0, areaSize)),
                             "Speed", StringValue("ns3::UniformRandomVariable[Min=2.0|Max=4.0]"),
                             "Mode", StringValue("Time"), "Time", StringValue("0.5s"), "Distance", StringValue("5.0"));
    mobility.Install(ues);

    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    Ptr<ListPositionAllocator> routerPos = CreateObject<ListPositionAllocator>();
    for (uint32_t i = 0; i < numRouters; ++i)
        routerPos->Add(Vector(20 + 20 * (i % 3), 20 + 20 * (i / 3), 0));
    mobility.SetPositionAllocator(routerPos);
    mobility.Install(routers);
    mobility.Install(servers);

    InternetStackHelper internet;
    internet.Install(ues);
    internet.Install(routers);
    internet.Install(servers);

    Ptr<mmwave::MmWaveHelper> mmwaveHelper = CreateObject<mmwave::MmWaveHelper>();
    NetDeviceContainer routerDevs = mmwaveHelper->InstallEnbDevice(routers);
    NetDeviceContainer ueDevs = mmwaveHelper->InstallUeDevice(ues);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ueIfaces = ipv4.Assign(ueDevs);
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer routerIfaces = ipv4.Assign(routerDevs);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("2ms"));
    ipv4.SetBase("10.2.1.0", "255.255.255.0");
    for (uint32_t i = 0; i < numRouters; ++i)
    {
        NetDeviceContainer p2pDevs = p2p.Install(routers.Get(i), servers.Get(0));
        ipv4.Assign(p2pDevs);
        ipv4.NewNetwork();
    }

    mmwaveHelper->AttachToClosestEnb(ueDevs, routerDevs);

    uint16_t port = 4000;
    std::vector<Ptr<Socket>> routerSockets;
    for (uint32_t i = 0; i < numRouters; ++i)
    {
        Ptr<Socket> sink = Socket::CreateSocket(routers.Get(i), UdpSocketFactory::GetTypeId());
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), port + i);
        sink->Bind(local);
        sink->SetRecvCallback(MakeCallback(&PacketSinkReceive));
        routerSockets.push_back(sink);
    }

    ueTrafficType.resize(numUes);
    for (uint32_t i = 0; i < numUes; ++i)
    {
        ueTrafficType[i] = std::uniform_int_distribution<int>(0, 2)(generator);
        Ptr<DynamicTrafficApp> app = CreateObject<DynamicTrafficApp>();
        Ptr<MobilityModel> ueMob = ues.Get(i)->GetObject<MobilityModel>();
        double minDist = std::numeric_limits<double>::max();
        uint32_t nearestRouter = 0;
        for (uint32_t j = 0; j < numRouters; ++j)
        {
            Ptr<MobilityModel> routerMob = routers.Get(j)->GetObject<MobilityModel>();
            double dist = ueMob->GetDistanceFrom(routerMob);
            if (dist < minDist)
            {
                minDist = dist;
                nearestRouter = j;
            }
        }
        Ipv4Address routerAddr = routers.Get(nearestRouter)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        app->Setup(InetSocketAddress(routerAddr, port + nearestRouter), i);
        ues.Get(i)->AddApplication(app);
        app->SetStartTime(Seconds(0.1 * i));
        app->SetStopTime(Seconds(simTime));
    }

    FlowMonitorHelper flowmonHelper;
    Ptr<FlowMonitor> monitor = flowmonHelper.InstallAll();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());

    Simulator::Schedule(Seconds(0.01), &SDNControllerCallback, monitor, classifier, ues);

    AnimationInterface anim("life.xml");
    anim.SetMaxPktsPerTraceFile(1000000);
    anim.UpdateNodeSize(0, nodeRadius * 1000, nodeRadius * 1000); // 3 cm in mm

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    csvFile.close();
    return 0;
}
