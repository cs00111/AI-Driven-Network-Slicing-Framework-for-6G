#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/mmwave-module.h"
#include "ns3/traffic-control-module.h"
#include <fstream>
#include <random>
#include <vector>
#include <queue>
#include <cmath>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("6GNetworkSlicing");

enum SliceType { eMBB, URLLC, mMTC };
const uint32_t numUes = 20;
const uint32_t numRouters = 6;
const double simTime = 10.0;
const double totalBandwidth = 100.0; // Mbps
const uint32_t totalQueueBytes = 1000000; // 1 MB
const double areaSize = 50.0;

std::default_random_engine generator(1234);
std::ofstream csvFile;
std::vector<bool> ueActive(numUes, false);
std::vector<uint32_t> packetCounts = {0, 0, 0}; // eMBB, URLLC, mMTC

class LatencyTag : public Tag
{
public:
    static TypeId GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::LatencyTag")
            .SetParent<Tag>()
            .AddConstructor<LatencyTag>();
        return tid;
    }
    TypeId GetInstanceTypeId(void) const override { return GetTypeId(); }
    uint32_t GetSerializedSize(void) const override { return 24; }
    void Serialize(TagBuffer i) const override 
    {
        i.WriteDouble(m_latencyRequirement);
        i.WriteDouble(m_packetRate);
        i.WriteDouble(m_throughputRequired);
    }
    void Deserialize(TagBuffer i) override 
    {
        m_latencyRequirement = i.ReadDouble();
        m_packetRate = i.ReadDouble();
        m_throughputRequired = i.ReadDouble();
    }
    void Print(std::ostream &os) const override 
    { 
        os << "Latency=" << m_latencyRequirement << ", Rate=" << m_packetRate << ", Throughput=" << m_throughputRequired; 
    }
    void SetLatency(double latency) { m_latencyRequirement = latency; }
    double GetLatency() const { return m_latencyRequirement; }
    void SetPacketRate(double rate) { m_packetRate = rate; }
    double GetPacketRate() const { return m_packetRate; }
    void SetThroughput(double throughput) { m_throughputRequired = throughput; }
    double GetThroughput() const { return m_throughputRequired; }

private:
    double m_latencyRequirement = 0.0;
    double m_packetRate = 0.0;
    double m_throughputRequired = 0.0;
};

class VirtualSlice
{
public:
    VirtualSlice(std::string name)
        : name(name), bandwidthMbps(0.0), maxQueueBytes(0), packetsDropped(0), packetsEnqueued(0),
          totalLatency(0.0), totalBytes(0.0), packetRate(0.0), throughputActual(0.0) {}
    
    bool Enqueue(Ptr<Packet> packet)
    {
        if (buffer.size() * packet->GetSize() >= maxQueueBytes)
        {
            packetsDropped++;
            return false;
        }
        buffer.push(packet);
        packetsEnqueued++;
        totalBytes += packet->GetSize();
        throughputActual = totalBytes * 8.0 / (Simulator::Now().GetSeconds() + 1e-6) / 1e6;
        return true;
    }
    
    void Resize(double newBw, uint32_t newQueueSize)
    {
        bandwidthMbps = newBw;
        maxQueueBytes = newQueueSize;
        NS_LOG_INFO("Slice " << name << " resized: BW=" << bandwidthMbps << " Mbps, Queue=" << maxQueueBytes << " bytes");
        csvFile << Simulator::Now().GetSeconds() << ",SLICE_RESIZE,-1,-1,-1,-1,-1,-1," << name << ",-1,-1," << bandwidthMbps << "," << GetQueueOccupancy() << "," << GetQueueBytes() << ",-1,-1\n";
    }
    
    double GetQueueOccupancy() const
    {
        return maxQueueBytes > 0 ? (buffer.size() * 1000.0 / maxQueueBytes) : 0.0;
    }
    
    uint32_t GetQueueBytes() const
    {
        return buffer.size() * 1000;
    }
    
    double GetDropRate() const
    {
        return packetsEnqueued > 0 ? packetsDropped / static_cast<double>(packetsEnqueued + packetsDropped) : 0.0;
    }

    std::string name;
    double bandwidthMbps;
    uint32_t maxQueueBytes;
    std::queue<Ptr<Packet>> buffer;
    uint32_t packetsDropped;
    uint32_t packetsEnqueued;
    double totalLatency;
    double totalBytes;
    double packetRate;
    double throughputActual;
};

class PacketGeneratorApp : public Application
{
public:
    PacketGeneratorApp() : m_socket(0), m_running(false), m_ueId(0), m_sliceType(eMBB), m_lastPacketTime(0.0), m_enabled(false) {}
    virtual ~PacketGeneratorApp() { m_socket = 0; }
    
    void Setup(Address peer, uint32_t ueId, SliceType slice)
    {
        m_peer = peer;
        m_ueId = ueId;
        m_sliceType = slice;
    }

    void Enable() { m_enabled = true; if (m_running && m_isOnline) SchedulePacket(); }
    void Disable() { m_enabled = false; Simulator::Cancel(m_sendEvent); }

private:
    void StartApplication(void) override
    {
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Connect(m_peer);
        if (m_enabled && m_isOnline) SchedulePacket();
        ScheduleStateTransition();
    }
    
    void StopApplication(void) override
    {
        m_running = false;
        Simulator::Cancel(m_sendEvent);
        Simulator::Cancel(m_stateEvent);
        if (m_socket) m_socket->Close();
    }
    
    void SchedulePacket(void)
    {
        if (!m_running || !m_isOnline || !m_enabled) return;
        
        Ptr<UniformRandomVariable> sizeRv = CreateObject<UniformRandomVariable>();
        double minSize, maxSize;
        if (m_sliceType == eMBB) { minSize = 10000; maxSize = 100000; }
        else if (m_sliceType == URLLC) { minSize = 10; maxSize = 500; }
        else { minSize = 10; maxSize = 200; }
        sizeRv->SetAttribute("Min", DoubleValue(minSize));
        sizeRv->SetAttribute("Max", DoubleValue(maxSize));
        uint32_t packetSize = static_cast<uint32_t>(sizeRv->GetValue());
        
        Ptr<UniformRandomVariable> latencyRv = CreateObject<UniformRandomVariable>();
        double minLatency, maxLatency;
        if (m_sliceType == eMBB) { minLatency = 0.01; maxLatency = 0.1; }
        else if (m_sliceType == URLLC) { minLatency = 0.0001; maxLatency = 0.01; }
        else { minLatency = 0.1; maxLatency = 1.0; }
        latencyRv->SetAttribute("Min", DoubleValue(minLatency));
        latencyRv->SetAttribute("Max", DoubleValue(maxLatency));
        double latency = latencyRv->GetValue();
        
        Ptr<Packet> packet = Create<Packet>(packetSize);
        LatencyTag tag;
        tag.SetLatency(latency);
        double now = Simulator::Now().GetSeconds();
        if (m_lastPacketTime > 0.0)
            m_packetRate = 1.0 / (now - m_lastPacketTime);
        tag.SetPacketRate(m_packetRate);
        m_throughputRequired = m_packetRate * packetSize * 8.0 / 1e6;
        tag.SetThroughput(m_throughputRequired);
        packet->AddPacketTag(tag);
        
        if (m_socket->Send(packet) >= 0)
        {
            m_lastPacketTime = now;
            NS_LOG_INFO("UE " << m_ueId << " sent packet: size=" << packetSize << ", latency=" << latency << ", rate=" << m_packetRate << ", throughput=" << m_throughputRequired);
            packetCounts[m_sliceType]++;
        }
        
        Ptr<ExponentialRandomVariable> intervalRv = CreateObject<ExponentialRandomVariable>();
        double meanInterval;
        if (m_sliceType == eMBB) meanInterval = 0.07; // ~14.3 packets/s
        else if (m_sliceType == URLLC) meanInterval = 0.116; // ~8.6 packets/s
        else meanInterval = 0.15; // ~6.7 packets/s
        intervalRv->SetAttribute("Mean", DoubleValue(meanInterval));
        double interval = intervalRv->GetValue();
        m_sendEvent = Simulator::Schedule(Seconds(interval), &PacketGeneratorApp::SchedulePacket, this);
    }
    
    void ScheduleStateTransition(void)
    {
        if (!m_running) return;
        Ptr<ExponentialRandomVariable> timeRv = CreateObject<ExponentialRandomVariable>();
        timeRv->SetAttribute("Mean", DoubleValue(m_isOnline ? 2.0 : 1.0));
        double duration = timeRv->GetValue();
        m_stateEvent = Simulator::Schedule(Seconds(duration), &PacketGeneratorApp::ToggleState, this);
    }
    
    void ToggleState(void)
    {
        m_isOnline = !m_isOnline;
        if (m_isOnline && m_enabled) SchedulePacket();
        else Simulator::Cancel(m_sendEvent);
        ScheduleStateTransition();
    }

    Ptr<Socket> m_socket;
    Address m_peer;
    bool m_running;
    uint32_t m_ueId;
    SliceType m_sliceType;
    bool m_isOnline = true;
    bool m_enabled;
    EventId m_sendEvent;
    EventId m_stateEvent;
    double m_lastPacketTime;
    double m_packetRate = 0.0;
    double m_throughputRequired = 0.0;
};

class SDNController
{
public:
    SDNController()
    {
        slices.emplace_back("eMBB");
        slices.emplace_back("URLLC");
        slices.emplace_back("mMTC");
    }
    
    SliceType ClassifyPacket(Ptr<Packet> packet)
    {
        double size = packet->GetSize();
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        double latency = tag.GetLatency();
        double packetRate = tag.GetPacketRate();
        double throughput = tag.GetThroughput();
        
        // Normalize parameters
        double normSize = (size - 10) / (100000 - 10);
        double normLatency = (latency - 0.0001) / (1.0 - 0.0001);
        double normRate = (packetRate - 0) / (200 - 0);
        double normThroughput = (throughput - 0) / (100 - 0);
        
        // Weighted scores
        double weights[4] = {0.3, 0.4, 0.2, 0.1}; // Size, Latency, Rate, Throughput
        double scores[3] = {0.0, 0.0, 0.0}; // eMBB, URLLC, mMTC
        
        // eMBB: Large size, moderate latency, moderate rate
        scores[0] = weights[0] * (normSize > 0.1 ? 1.0 : normSize / 0.1) +
                   weights[1] * (normLatency > 0.01 && normLatency < 0.1 ? 1.0 : 0.5) +
                   weights[2] * (normRate > 0.05 && normRate < 0.5 ? 1.0 : 0.5) +
                   weights[3] * normThroughput;
        
        // URLLC: Small size, low latency, high rate
        scores[1] = weights[0] * (normSize < 0.005 ? 1.0 : 0.005 / normSize) +
                   weights[1] * (normLatency < 0.01 ? 1.0 : 0.01 / normLatency) +
                   weights[2] * (normRate > 0.25 ? 1.0 : normRate / 0.25) +
                   weights[3] * normThroughput;
        
        // mMTC: Small size, high latency, low rate
        scores[2] = weights[0] * (normSize < 0.002 ? 1.0 : 0.002 / normSize) +
                   weights[1] * (normLatency > 0.1 ? 1.0 : normLatency / 0.1) +
                   weights[2] * (normRate < 0.1 ? 1.0 : 0.1 / normRate) +
                   weights[3] * normThroughput;
        
        // Select slice with highest score
        SliceType slice = eMBB;
        double maxScore = scores[0];
        if (scores[1] > maxScore) { maxScore = scores[1]; slice = URLLC; }
        if (scores[2] > maxScore) { slice = mMTC; }
        
        return slice;
    }
    
    void AllocateResources(Ptr<Packet> packet, SliceType slice)
    {
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        double throughputReq = tag.GetThroughput();
        uint32_t queueReq = packet->GetSize() * 2;
        
        double totalAllocatedBw = 0.0;
        uint32_t totalAllocatedQueue = 0;
        for (const auto& s : slices)
        {
            totalAllocatedBw += s.bandwidthMbps;
            totalAllocatedQueue += s.maxQueueBytes;
        }
        
        double availableBw = totalBandwidth - totalAllocatedBw;
        uint32_t availableQueue = totalQueueBytes - totalAllocatedQueue;
        
        double bwShare = std::min(throughputReq, availableBw);
        uint32_t queueShare = std::min(queueReq, availableQueue);
        
        slices[slice].Resize(slices[slice].bandwidthMbps + bwShare, slices[slice].maxQueueBytes + queueShare);
        
        double totalBw = 0.0;
        uint32_t totalQueue = 0;
        for (const auto& s : slices)
        {
            totalBw += s.bandwidthMbps;
            totalQueue += s.maxQueueBytes;
        }
        if (totalBw > totalBandwidth || totalQueue > totalQueueBytes)
        {
            for (auto& s : slices)
            {
                s.bandwidthMbps *= totalBandwidth / (totalBw + 1e-6);
                s.maxQueueBytes = static_cast<uint32_t>(s.maxQueueBytes * totalQueueBytes / (totalQueue + 1e-6));
            }
        }
        
        // Check packet distribution
        uint32_t totalPackets = packetCounts[0] + packetCounts[1] + packetCounts[2];
        if (totalPackets > 0)
        {
            double ratios[3] = {
                packetCounts[0] / (double)totalPackets,
                packetCounts[1] / (double)totalPackets,
                packetCounts[2] / (double)totalPackets
            };
            if (ratios[0] < 0.45 || ratios[0] > 0.55 || ratios[1] < 0.25 || ratios[1] > 0.35 || ratios[2] < 0.15 || ratios[2] > 0.25)
            {
                NS_LOG_INFO("Packet distribution off: eMBB=" << ratios[0]*100 << "%, URLLC=" << ratios[1]*100 << "%, mMTC=" << ratios[2]*100 << "%");
            }
        }
    }
    
    bool ProcessPacket(Ptr<Packet> packet, Time arrivalTime, double compTime)
    {
        SliceType slice = ClassifyPacket(packet);
        AllocateResources(packet, slice);
        bool enqueued = slices[slice].Enqueue(packet);
        
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        double latencyActual = (Simulator::Now() - arrivalTime).GetSeconds() + compTime;
        slices[slice].totalLatency += latencyActual;
        slices[slice].packetRate = tag.GetPacketRate();
        
        double reward = CalculateReward(slice, latencyActual, tag.GetLatency(), enqueued);
        
        csvFile << Simulator::Now().GetSeconds() << "," << packet->GetUid() << "," << packet->GetSize() << ","
                << tag.GetLatency() << "," << latencyActual << "," << tag.GetPacketRate() << "," << tag.GetThroughput() << ","
                << slices[slice].throughputActual << "," << slices[slice].name << "," << enqueued << ","
                << (!enqueued) << "," << slices[slice].bandwidthMbps << "," << slices[slice].GetQueueOccupancy() << ","
                << slices[slice].GetQueueBytes() << "," << reward << "," << compTime << "\n";
        
        return enqueued;
    }
    
    double CalculateReward(SliceType slice, double latencyActual, double latencyReq, bool enqueued)
    {
        double latencyScore = latencyActual <= latencyReq ? 1.0 : -1.0 * (latencyActual / latencyReq);
        double throughputScore = slices[slice].throughputActual >= slices[slice].packetRate * 1000 * 8 / 1e6 ? 1.0 : -1.0;
        double dropScore = enqueued ? 1.0 : -2.0;
        return 0.5 * latencyScore + 0.3 * throughputScore + 0.2 * dropScore;
    }

    std::vector<VirtualSlice> slices;
};

class PacketReceiverApp : public Application
{
public:
    PacketReceiverApp(SDNController* controller) : m_socket(0), m_running(false), m_controller(controller) {}
    virtual ~PacketReceiverApp() { m_socket = 0; }
    
    void StartApplication(void) override
    {
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), 5000));
        m_socket->SetRecvCallback(MakeCallback(&PacketReceiverApp::HandleReceive, this));
    }
    
    void StopApplication(void) override
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
            Ptr<UniformRandomVariable> compTimeRv = CreateObject<UniformRandomVariable>();
            compTimeRv->SetAttribute("Min", DoubleValue(0.0001)); // 0.1 ms
            compTimeRv->SetAttribute("Max", DoubleValue(0.001)); // 1 ms
            double compTime = compTimeRv->GetValue();
            m_controller->ProcessPacket(packet, Simulator::Now(), compTime);
        }
    }

    Ptr<Socket> m_socket;
    bool m_running;
    SDNController* m_controller;
};

void UpdateActiveUes(std::vector<Ptr<PacketGeneratorApp>>& apps)
{
    Ptr<UniformRandomVariable> activeUeRv = CreateObject<UniformRandomVariable>();
    activeUeRv->SetAttribute("Min", DoubleValue(10));
    activeUeRv->SetAttribute("Max", DoubleValue(20));
    uint32_t activeCount = static_cast<uint32_t>(activeUeRv->GetValue());
    
    std::vector<uint32_t> indices(numUes);
    for (uint32_t i = 0; i < numUes; ++i) indices[i] = i;
    std::shuffle(indices.begin(), indices.end(), generator);
    
    for (uint32_t i = 0; i < numUes; ++i)
    {
        if (i < activeCount)
        {
            ueActive[indices[i]] = true;
            apps[indices[i]]->Enable();
        }
        else
        {
            ueActive[indices[i]] = false;
            apps[indices[i]]->Disable();
        }
    }
    
    NS_LOG_INFO("Active UEs updated: " << activeCount);
    Simulator::Schedule(Seconds(2.0), &UpdateActiveUes, apps);
}

int main(int argc, char *argv[])
{
    uint32_t seed = 1234;
    CommandLine cmd;
    cmd.AddValue("seed", "Random seed", seed);
    cmd.Parse(argc, argv);
    generator.seed(seed);
    
    LogComponentEnable("6GNetworkSlicing", LOG_LEVEL_INFO);
    
    csvFile.open("network_slicing.csv");
    csvFile << "Time,Packet_ID,PacketSize,LatencyReq,LatencyActual,PacketRate,ThroughputReq,ThroughputActual,Slice,Enqueued,Dropped,Slice_BW,Queue_Occupancy,Queue_Bytes,Reward,CompTime\n";
    
    NodeContainer ues, routers, baseStation, server;
    ues.Create(numUes);
    routers.Create(numRouters);
    baseStation.Create(1);
    server.Create(1);
    
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                             "Bounds", RectangleValue(Rectangle(0, areaSize, 0, areaSize)),
                             "Speed", StringValue("ns3::UniformRandomVariable[Min=1.0|Max=5.0]"));
    mobility.Install(ues);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    Ptr<ListPositionAllocator> routerPos = CreateObject<ListPositionAllocator>();
    for (uint32_t i = 0; i < numRouters; ++i)
        routerPos->Add(Vector(20 + 20 * (i % 3), 20 + 20 * (i / 3), 0));
    mobility.SetPositionAllocator(routerPos);
    mobility.Install(routers);
    mobility.Install(baseStation);
    mobility.Install(server);
    
    InternetStackHelper internet;
    internet.Install(ues);
    internet.Install(routers);
    internet.Install(baseStation);
    internet.Install(server);
    
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
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    ipv4.SetBase("10.2.1.0", "255.255.255.0");
    NetDeviceContainer bsDevs, serverDevs;
    for (uint32_t i = 0; i < numRouters; ++i)
    {
        NetDeviceContainer p2pDevs = p2p.Install(routers.Get(i), baseStation.Get(0));
        bsDevs.Add(p2pDevs.Get(1));
        ipv4.Assign(p2pDevs);
        ipv4.NewNetwork();
    }
    NetDeviceContainer serverP2pDevs = p2p.Install(baseStation.Get(0), server.Get(0));
    serverDevs.Add(serverP2pDevs.Get(1));
    ipv4.Assign(serverP2pDevs);
    
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::PfifoFastQueueDisc");
    tch.Install(ueDevs);
    tch.Install(routerDevs);
    
    mmwaveHelper->AttachToClosestEnb(ueDevs, routerDevs);
    
    for (uint32_t i = 0; i < ueDevs.GetN(); ++i)
    {
        Ptr<mmwave::MmWaveUeNetDevice> ueDev = DynamicCast<mmwave::MmWaveUeNetDevice>(ueDevs.Get(i));
        NS_LOG_INFO("UE " << i << " device created: " << (ueDev ? "valid" : "invalid"));
    }
    
    SDNController controller;
    Ptr<PacketReceiverApp> receiver = CreateObject<PacketReceiverApp>(&controller);
    server.Get(0)->AddApplication(receiver);
    receiver->SetStartTime(Seconds(0.0));
    receiver->SetStopTime(Seconds(simTime));
    
    std::vector<Ptr<PacketGeneratorApp>> apps;
    for (uint32_t i = 0; i < numUes; ++i)
    {
        SliceType slice = static_cast<SliceType>(i % 3);
        Ptr<PacketGeneratorApp> app = CreateObject<PacketGeneratorApp>();
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
        Ipv4Address routerAddr = routerIfaces.GetAddress(nearestRouter);
        app->Setup(InetSocketAddress(routerAddr, 5000), i, slice);
        ues.Get(i)->AddApplication(app);
        app->SetStartTime(Seconds(0.1 * i));
        app->SetStopTime(Seconds(simTime));
        apps.push_back(app);
    }
    
    UpdateActiveUes(apps);
    
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();
    
    csvFile.close();
    NS_LOG_INFO("Packet counts: eMBB=" << packetCounts[0] << ", URLLC=" << packetCounts[1] << ", mMTC=" << packetCounts[2]);
    return 0;
}
