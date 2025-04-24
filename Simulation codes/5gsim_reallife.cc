#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/mmwave-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/netanim-module.h"
#include "ns3/traffic-control-module.h"
#include <fstream>
#include <random>
#include <vector>
#include <map>
#include <unordered_map>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("DynamicTrafficApp");

enum TrafficType { eMBB = 0, URLLC = 1, mMTC = 2 };
const uint32_t numUes = 40;
const uint32_t numRouters = 6;
const double simTime = 20.0;
const double sdnInterval = 0.01;
const double areaSize = 50.0;
const double nodeRadius = 0.3;
const double routerBandwidth = 1e9; // 1 Gbps
const double ueSpeedRange[2] = {2.0, 4.0}; // m/s
const double meanOnTime = 5.0; // UE online
const double meanOffTime = 2.0; // UE offline
const double ackSize = 50; // ACK packet size
const double congestionRate = 10e6; // 10 Mbps bursts
const double congestionDuration = 0.1; // 0.1s bursts
const double serverDelayMean = 0.001; // 1ms
const double computePerKb = 0.001; // Reduced for realistic CPU/memory usage
const double learningRate = 0.1;
const double discountFactor = 0.9;
const double epsilon = 0.1;

std::default_random_engine generator;
std::ofstream csvFile, stateCsvFile;
std::vector<int> ueTrafficType;
std::vector<double> sliceBandwidth = {0.5, 0.3, 0.2}; // eMBB, URLLC, mMTC
std::vector<double> sliceCpuLimit = {100.0, 50.0, 200.0}; // eMBB, URLLC, mMTC
std::vector<double> sliceMemoryLimit = {100.0, 50.0, 200.0};
std::unordered_map<uint32_t, double> qTable; // state-action pairs
int currentAction = 3;

class DynamicTrafficApp : public Application
{
public:
    DynamicTrafficApp() : m_socket(0), m_ackSocket(0), m_running(false), m_ueId(0), m_lastPacketTime(0.0), 
                         m_queueSize(0), m_isOnline(true), m_isTrafficActive(true), m_cpuUsage(0.0), m_memoryUsage(0.0) {}
    virtual ~DynamicTrafficApp() { m_socket = 0; m_ackSocket = 0; }
    void Setup(Address txAddress, Address rxAddress, uint16_t rxPort, uint32_t ueId)
    {
        m_peer = txAddress;
        m_localAddress = rxAddress;
        m_localPort = rxPort;
        m_ueId = ueId;
    }
    double GetLastPacketTime() const { return m_lastPacketTime; }
    uint32_t GetQueueSize() const { return m_queueSize; }
    bool IsOnline() const { return m_isOnline; }
    double GetCpuUsage() const { return m_cpuUsage; }
    double GetMemoryUsage() const { return m_memoryUsage; }

private:
    virtual void StartApplication(void)
    {
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Connect(m_peer);
        m_ackSocket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_ackSocket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_localPort));
        m_ackSocket->SetRecvCallback(MakeCallback(&DynamicTrafficApp::HandleAck, this));
        SchedulePacket();
        ScheduleStateTransition();
        ScheduleTrafficToggle();
    }
    virtual void StopApplication(void)
    {
        m_running = false;
        if (m_sendEvent.IsPending()) Simulator::Cancel(m_sendEvent);
        if (m_stateEvent.IsPending()) Simulator::Cancel(m_stateEvent);
        if (m_trafficEvent.IsPending()) Simulator::Cancel(m_trafficEvent);
        if (m_socket) m_socket->Close();
        if (m_ackSocket) m_ackSocket->Close();
    }
    void SchedulePacket(void)
    {
        if (!m_running || !m_isOnline || !m_isTrafficActive) return;
        int sliceId = ueTrafficType[m_ueId];
        uint32_t packetSize = 0;
        double interval = 0.0;
        // Fixed packet sizes for realism
        if (sliceId == eMBB)
        {
            std::uniform_int_distribution<int> sizeDist(1000, 1500); // 1000–1500 bytes
            packetSize = sizeDist(generator);
            std::exponential_distribution<double> rateDist(100.0); // Higher frequency
            interval = rateDist(generator) / 1000.0;
        }
        else if (sliceId == URLLC)
        {
            std::uniform_int_distribution<int> sizeDist(300, 600); // 300–600 bytes
            packetSize = sizeDist(generator);
            std::poisson_distribution<int> rateDist(200);
            interval = rateDist(generator) / 1000.0;
        }
        else // mMTC
        {
            std::uniform_int_distribution<int> sizeDist(50, 200); // 50–200 bytes
            packetSize = sizeDist(generator);
            std::exponential_distribution<double> rateDist(10.0);
            interval = rateDist(generator) / 1000.0;
        }
        m_queueSize++;
        // Cap CPU/memory usage
        m_cpuUsage = std::min(m_cpuUsage + packetSize / 1000.0 * computePerKb, sliceCpuLimit[sliceId]);
        m_memoryUsage = std::min(m_memoryUsage + packetSize / 1000.0 * computePerKb, sliceMemoryLimit[sliceId]);
        Ptr<Packet> packet = Create<Packet>(packetSize);
        if (m_socket->Send(packet) >= 0)
        {
            m_lastPacketTime = Simulator::Now().GetSeconds();
            NS_LOG_INFO("UE " << m_ueId << " sent packet of size " << packetSize << " at " << m_lastPacketTime);
        }
        else
        {
            NS_LOG_ERROR("UE " << m_ueId << " failed to send packet");
        }
        if (m_running && m_isOnline && m_isTrafficActive)
            m_sendEvent = Simulator::Schedule(Seconds(interval), &DynamicTrafficApp::SchedulePacket, this);
    }
    void ScheduleStateTransition(void)
    {
        if (!m_running) return;
        double duration = m_isOnline ? 
            std::exponential_distribution<double>(1.0 / meanOnTime)(generator) :
            std::exponential_distribution<double>(1.0 / meanOffTime)(generator);
        m_stateEvent = Simulator::Schedule(Seconds(duration), &DynamicTrafficApp::ToggleState, this);
    }
    void ToggleState(void)
    {
        m_isOnline = !m_isOnline;
        // Log to state.csv
        stateCsvFile << Simulator::Now().GetSeconds() << ",STATE_CHANGE," << m_ueId << ",-1,-1,-1,-1,-1\n";
        if (m_isOnline && m_isTrafficActive)
            SchedulePacket();
        ScheduleStateTransition();
    }
    void ScheduleTrafficToggle(void)
    {
        if (!m_running) return;
        double meanOn, meanOff;
        int sliceId = ueTrafficType[m_ueId];
        if (sliceId == eMBB) { meanOn = 10.0; meanOff = 2.0; }
        else if (sliceId == URLLC) { meanOn = 1.0; meanOff = 0.5; }
        else { meanOn = 20.0; meanOff = 5.0; }
        double duration = m_isTrafficActive ? 
            std::exponential_distribution<double>(1.0 / meanOn)(generator) :
            std::exponential_distribution<double>(1.0 / meanOff)(generator);
        m_trafficEvent = Simulator::Schedule(Seconds(duration), &DynamicTrafficApp::ToggleTraffic, this);
    }
    void ToggleTraffic(void)
    {
        m_isTrafficActive = !m_isTrafficActive;
        if (m_isOnline && m_isTrafficActive)
            SchedulePacket();
        ScheduleTrafficToggle();
    }
    void HandleAck(Ptr<Socket> socket)
    {
        Ptr<Packet> packet;
        while ((packet = socket->Recv()))
        {
            m_queueSize = std::max(0, static_cast<int>(m_queueSize) - 1);
            NS_LOG_INFO("UE " << m_ueId << " received ACK, queue size now " << m_queueSize);
        }
    }

    Ptr<Socket> m_socket;
    Ptr<Socket> m_ackSocket;
    Address m_peer;
    Address m_localAddress;
    uint16_t m_localPort;
    EventId m_sendEvent;
    EventId m_stateEvent;
    EventId m_trafficEvent;
    bool m_running;
    uint32_t m_ueId;
    double m_lastPacketTime;
    uint32_t m_queueSize;
    bool m_isOnline;
    bool m_isTrafficActive;
    double m_cpuUsage;
    double m_memoryUsage;
};

class CongestionApp : public Application
{
public:
    CongestionApp() : m_socket(0), m_running(false) {}
    virtual ~CongestionApp() { m_socket = 0; }
    void Setup(Address peer)
    {
        m_peer = peer;
    }
private:
    virtual void StartApplication(void)
    {
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Connect(m_peer);
        ScheduleBurst();
    }
    virtual void StopApplication(void)
    {
        m_running = false;
        if (m_sendEvent.IsPending()) Simulator::Cancel(m_sendEvent);
        if (m_socket) m_socket->Close();
    }
    void ScheduleBurst(void)
    {
        if (!m_running) return;
        std::exponential_distribution<double> intervalDist(1.0 / 1.0);
        double interval = intervalDist(generator);
        m_sendEvent = Simulator::Schedule(Seconds(interval), &CongestionApp::SendBurst, this);
    }
    void SendBurst(void)
    {
        uint32_t packetSize = 1000; // 1 KB
        uint32_t numPackets = static_cast<uint32_t>(congestionRate * congestionDuration / (packetSize * 8));
        for (uint32_t i = 0; i < numPackets; ++i)
        {
            Ptr<Packet> packet = Create<Packet>(packetSize);
            m_socket->Send(packet);
        }
        NS_LOG_INFO("Congestion burst sent with " << numPackets << " packets");
        ScheduleBurst();
    }

    Ptr<Socket> m_socket;
    Address m_peer;
    EventId m_sendEvent;
    bool m_running;
};

class ServerApp : public Application
{
public:
    ServerApp() : m_socket(0), m_running(false), m_cpuUsage(0.0), m_memoryUsage(0.0) {}
    virtual ~ServerApp() { m_socket = 0; }
    double GetCpuUsage() const { return m_cpuUsage; }
    double GetMemoryUsage() const { return m_memoryUsage; }
private:
    virtual void StartApplication(void)
    {
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), 5000));
        m_socket->SetRecvCallback(MakeCallback(&ServerApp::HandleReceive, this));
    }
    virtual void StopApplication(void)
    {
        m_running = false;
        if (m_socket) m_socket->Close();
    }
    void HandleReceive(Ptr<Socket> socket)
    {
        Ptr<Packet> packet;
        Address from;
        while ((packet = socket->RecvFrom(from)))
        {
            std::exponential_distribution<double> delayDist(1.0 / serverDelayMean);
            Simulator::Schedule(Seconds(delayDist(generator)), &ServerApp::ProcessPacket, this, packet);
            m_cpuUsage = std::min(m_cpuUsage + packet->GetSize() / 1000.0 * computePerKb, 300.0);
            m_memoryUsage = std::min(m_memoryUsage + packet->GetSize() / 1000.0 * computePerKb, 300.0);
            InetSocketAddress inetAddr = InetSocketAddress::ConvertFrom(from);
            NS_LOG_INFO("Server received packet of size " << packet->GetSize() << " from " << inetAddr.GetIpv4());
        }
    }
    void ProcessPacket(Ptr<Packet> packet)
    {
        // Simulate processing
    }

    Ptr<Socket> m_socket;
    bool m_running;
    double m_cpuUsage;
    double m_memoryUsage;
};

class AckHandler : public Object
{
public:
    AckHandler(Ptr<Node> router, uint32_t routerId, uint16_t ackPort)
        : m_router(router), m_routerId(routerId), m_ackPort(ackPort) {}

    void HandleAck(Ptr<Socket> socket)
    {
        Ptr<Packet> packet;
        Address from;
        while ((packet = socket->RecvFrom(from)))
        {
            InetSocketAddress inetAddr = InetSocketAddress::ConvertFrom(from);
            Ptr<Socket> ackSocket = Socket::CreateSocket(m_router, UdpSocketFactory::GetTypeId());
            ackSocket->Connect(InetSocketAddress(inetAddr.GetIpv4(), m_ackPort));
            ackSocket->Send(Create<Packet>(ackSize));
            ackSocket->Close();
            NS_LOG_INFO("Router " << m_routerId << " sent ACK to " << inetAddr.GetIpv4());
        }
    }

private:
    Ptr<Node> m_router;
    uint32_t m_routerId;
    uint16_t m_ackPort;
};

int getState(const std::map<int, double>& trafficStats, double serverCpu, double serverMemory)
{
    double eMBB = trafficStats.at(0);
    double URLLC = trafficStats.at(1);
    double mMTC = trafficStats.at(2);
    bool serverOverloaded = (serverCpu > sliceCpuLimit[0] * 2 || serverMemory > sliceMemoryLimit[0] * 2);
    if (serverOverloaded) return 3; // S4: Server Overloaded
    if (eMBB > std::max(URLLC, mMTC) * 1.5 && eMBB > 1.0) return 0; // S1: eMBB Dominant
    if (URLLC > std::max(eMBB, mMTC) * 1.5 && URLLC > 0.5) return 1; // S2: URLLC Dominant
    return 2; // S3: mMTC Dominant or unclear
}

void UpdateQTable(int state, int action, double reward, int nextState)
{
    uint32_t stateAction = (state << 2) | action;
    double maxQ = 0.0;
    for (int a = 0; a < 4; ++a)
    {
        uint32_t sa = (nextState << 2) | a;
        if (qTable.find(sa) != qTable.end() && qTable[sa] > maxQ)
        {
            maxQ = qTable[sa];
        }
    }
    if (qTable.find(stateAction) == qTable.end()) qTable[stateAction] = 0.0;
    qTable[stateAction] += learningRate * (reward + discountFactor * maxQ - qTable[stateAction]);
    NS_LOG_INFO("Updated Q-table: State " << state << ", Action " << action << ", Q-value " << qTable[stateAction]);
}

int ChooseAction(int state)
{
    if (std::uniform_real_distribution<double>(0, 1)(generator) < epsilon)
        return std::uniform_int_distribution<int>(0, 3)(generator);
    double maxQ = -std::numeric_limits<double>::max();
    int bestAction = 0;
    for (int a = 0; a < 4; ++a)
    {
        uint32_t sa = (state << 2) | a;
        double q = (qTable.find(sa) != qTable.end()) ? qTable[sa] : 0.0;
        if (q > maxQ)
        {
            maxQ = q;
            bestAction = a;
        }
    }
    return bestAction;
}

double computeReward(int state, double throughput, double latency, uint32_t queueSize, double packetLoss)
{
    double reward = 0.0;
    if (state == 0) // eMBB Dominant
    {
        reward = throughput / 50.0 - latency / 100.0 - packetLoss * 10.0;
    }
    else if (state == 1) // URLLC Dominant
    {
        reward = -latency / 10.0 - packetLoss * 20.0 + (queueSize < 5 ? 5.0 : -5.0);
    }
    else if (state == 2) // mMTC Dominant
    {
        reward = throughput / 100.0 - (queueSize > 10 ? 5.0 : 0.0);
    }
    else // Server Overloaded
    {
        reward = -20.0 - latency / 50.0 - packetLoss * 15.0;
    }
    return reward;
}

void SDNControllerCallback(Ptr<FlowMonitor> monitor, Ptr<Ipv4FlowClassifier> classifier, 
                         NodeContainer ues, NodeContainer routers, Ptr<Node> server)
{
    monitor->CheckForLostPackets();
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();
    std::map<int, double> trafficStats = {{0, 0.0}, {1, 0.0}, {2, 0.0}};
    double totalThroughput = 0.0, totalLatency = 0.0, totalPacketLoss = 0.0, totalQueueLength = 0.0;

    // Aggregate queue lengths in KB
    for (uint32_t i = 0; i < routers.GetN(); ++i)
    {
        Ptr<TrafficControlLayer> tc = routers.Get(i)->GetObject<TrafficControlLayer>();
        if (tc)
        {
            Ptr<QueueDisc> queueDisc = tc->GetRootQueueDiscOnDevice(routers.Get(i)->GetDevice(1));
            if (queueDisc)
            {
                totalQueueLength += queueDisc->GetNBytes() / 1000.0; // Convert to KB
                NS_LOG_INFO("Router " << i << " queue length: " << queueDisc->GetNBytes() / 1000.0 << " KB");
            }
        }
    }

    for (uint32_t i = 0; i < ues.GetN(); ++i)
    {
        Ptr<DynamicTrafficApp> app = DynamicCast<DynamicTrafficApp>(ues.Get(i)->GetApplication(0));
        if (!app || !app->IsOnline()) continue;
        int sliceId = ueTrafficType[i];
        double throughput = 0.0, latency = 0.0, packetLoss = 0.0;
        Ptr<Ipv4> ipv4 = ues.Get(i)->GetObject<Ipv4>();
        if (!ipv4) continue;
        Ipv4Address ueAddr = ipv4->GetAddress(1, 0).GetLocal();
        for (auto it = stats.begin(); it != stats.end(); ++it)
        {
            Ipv4FlowClassifier::FiveTuple tuple = classifier->FindFlow(it->first);
            if (tuple.sourceAddress == ueAddr)
            {
                double duration = it->second.timeLastRxPacket.GetSeconds() - it->second.timeFirstTxPacket.GetSeconds();
                if (duration > 0 && it->second.rxPackets > 0)
                {
                    throughput = it->second.rxBytes * 8.0 / duration / 1e6; // Mbps
                    latency = it->second.delaySum.GetMilliSeconds() / it->second.rxPackets;
                    packetLoss = it->second.lostPackets / static_cast<double>(it->second.txPackets + it->second.lostPackets);
                }
                else
                {
                    NS_LOG_WARN("No valid flow stats for UE " << i);
                }
                break;
            }
        }
        uint32_t queueSize = app->GetQueueSize();
        double resourceAllocation = sliceBandwidth[sliceId] * routerBandwidth / numRouters / 1e6;
        double reward = computeReward(sliceId, throughput, latency, queueSize, packetLoss);

        trafficStats[sliceId] += throughput;
        totalThroughput += throughput;
        totalLatency += latency;
        totalPacketLoss += packetLoss;

        // Log UE metrics to life.csv
        csvFile << Simulator::Now().GetSeconds() << "," << sliceId << "," << i << ","
                << app->GetLastPacketTime() << "," << queueSize * 1500 << ","
                << throughput << "," << latency << "," << queueSize << ","
                << resourceAllocation << "," << reward << "," << (app->IsOnline() ? "ONLINE" : "OFFLINE") << ","
                << sliceBandwidth[sliceId] << "," << totalQueueLength << "\n";
    }

    Ptr<ServerApp> serverApp = DynamicCast<ServerApp>(server->GetApplication(0));
    double serverCpu = serverApp ? serverApp->GetCpuUsage() : 0.0;
    double serverMemory = serverApp ? serverApp->GetMemoryUsage() : 0.0;

    int state = getState(trafficStats, serverCpu, serverMemory);
    int nextAction = ChooseAction(state);
    double reward = computeReward(state, totalThroughput, totalLatency, 0, totalPacketLoss);

    if (state == 0) // eMBB Dominant
    {
        sliceBandwidth = {0.6, 0.2, 0.2};
    }
    else if (state == 1) // URLLC Dominant
    {
        sliceBandwidth = {0.2, 0.6, 0.2};
    }
    else if (state == 2) // mMTC Dominant
    {
        sliceBandwidth = {0.2, 0.2, 0.6};
    }
    else // Server Overloaded
    {
        sliceBandwidth = {0.33, 0.33, 0.34};
    }

    UpdateQTable(state, currentAction, reward, state);
    currentAction = nextAction;

    // Log state metrics to state.csv
    stateCsvFile << Simulator::Now().GetSeconds() << ",STATE,-1," << reward << "," << serverCpu << "," 
                 << serverMemory << "," << sliceBandwidth[0] << "," << totalQueueLength << "\n";

    Simulator::Schedule(Seconds(sdnInterval), &SDNControllerCallback, monitor, classifier, ues, routers, server);
}

int main(int argc, char *argv[])
{
    uint32_t seed = 1234;
    CommandLine cmd;
    cmd.AddValue("seed", "Random seed", seed);
    cmd.Parse(argc, argv);
    generator.seed(seed);

    LogComponentEnable("DynamicTrafficApp", LOG_LEVEL_INFO);

    csvFile.open("life.csv");
    csvFile << "Time Step,Slice ID,UE ID,Packet Arrival Time (s),Packet Size (bytes),Throughput (Mbps),"
            << "Latency (ms),Queue Size,Resource Allocation (Mbps),Reward,UE State,"
            << "Slice Bandwidth,Queue Length (KB)\n";

    stateCsvFile.open("state.csv");
    stateCsvFile << "Time,Type,UE_ID,Reward,Server_CPU,Server_Memory,Slice_Bandwidth_eMBB,Queue_Length_KB\n";

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
    NetDeviceContainer serverDevs;
    for (uint32_t i = 0; i < numRouters; ++i)
    {
        NetDeviceContainer p2pDevs = p2p.Install(routers.Get(i), servers.Get(0));
        serverDevs.Add(p2pDevs.Get(1));
        ipv4.Assign(p2pDevs);
        ipv4.NewNetwork();
    }

    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::PfifoFastQueueDisc");
    tch.Install(ueDevs);
    tch.Install(routerDevs);

    mmwaveHelper->AttachToClosestEnb(ueDevs, routerDevs);

    // Verify UE attachments using GetCellId (compatible with NS-3.42 mmWave)
    for (uint32_t i = 0; i < ueDevs.GetN(); ++i)
    {
        Ptr<mmwave::MmWaveUeNetDevice> ueDev = DynamicCast<mmwave::MmWaveUeNetDevice>(ueDevs.Get(i));
        if (ueDev->GetCellId() > 0)
            NS_LOG_INFO("UE " << i << " attached to Cell ID " << ueDev->GetCellId());
        else
            NS_LOG_WARN("UE " << i << " not attached to any eNB");
    }

    uint16_t port = 4000;
    uint16_t ackPort = 6000;
    std::vector<Ptr<AckHandler>> ackHandlers;
    for (uint32_t i = 0; i < numRouters; ++i)
    {
        Ptr<Socket> sink = Socket::CreateSocket(routers.Get(i), UdpSocketFactory::GetTypeId());
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), port + i);
        sink->Bind(local);
        
        Ptr<AckHandler> handler = CreateObject<AckHandler>(routers.Get(i), i, ackPort + i);
        sink->SetRecvCallback(MakeCallback(&AckHandler::HandleAck, handler));
        ackHandlers.push_back(handler);

        Ptr<CongestionApp> congApp = CreateObject<CongestionApp>();
        congApp->Setup(InetSocketAddress(servers.Get(0)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal(), 5000));
        routers.Get(i)->AddApplication(congApp);
        congApp->SetStartTime(Seconds(0.0));
        congApp->SetStopTime(Seconds(simTime));
    }

    Ptr<ServerApp> serverApp = CreateObject<ServerApp>();
    servers.Get(0)->AddApplication(serverApp);
    serverApp->SetStartTime(Seconds(0.0));
    serverApp->SetStopTime(Seconds(simTime));

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
        Ipv4Address ueAddr = ueIfaces.GetAddress(i);
        app->Setup(InetSocketAddress(routerAddr, port + nearestRouter), 
                  InetSocketAddress(ueAddr, ackPort + nearestRouter), 
                  ackPort + nearestRouter, i);
        ues.Get(i)->AddApplication(app);
        app->SetStartTime(Seconds(0.1 * i));
        app->SetStopTime(Seconds(simTime));
    }

    FlowMonitorHelper flowmonHelper;
    Ptr<FlowMonitor> monitor = flowmonHelper.InstallAll();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());

    Simulator::Schedule(Seconds(0.01), &SDNControllerCallback, monitor, classifier, ues, routers, servers.Get(0));
    AnimationInterface anim("life.xml");
    anim.SetMaxPktsPerTraceFile(1000000);
    for (uint32_t i = 0; i < numUes; ++i)
    {
        int sliceId = ueTrafficType[i];
        if (sliceId == 0) // eMBB: Red
            anim.UpdateNodeColor(ues.Get(i), 255, 0, 0);
        else if (sliceId == 1) // URLLC: Green
            anim.UpdateNodeColor(ues.Get(i), 0, 255, 0);
        else // mMTC: Blue
            anim.UpdateNodeColor(ues.Get(i), 0, 0, 255);
    }

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    csvFile.close();
    stateCsvFile.close();
    return 0;
}
