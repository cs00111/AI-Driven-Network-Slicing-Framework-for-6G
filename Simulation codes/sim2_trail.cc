#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/mmwave-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/netanim-module.h"
#include <fstream>
#include <random>
#include <vector>
#include <queue>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("6GNetworkSlicing");

enum SliceType { eMBB, URLLC, mMTC };
const uint32_t numUes = 20;
const uint32_t numRouters = 6;
const double simTime = 20.0;
const double monitorInterval = 0.5;
const double totalBandwidth = 100.0; // Mbps
const uint32_t totalQueueBytes = 1000000; // 1 MB
const double areaSize = 50.0;

std::default_random_engine generator(1234);
std::ofstream csvFile;

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
    uint32_t GetSerializedSize(void) const override { return 8; }
    void Serialize(TagBuffer i) const override { i.WriteDouble(m_latencyRequirement); }
    void Deserialize(TagBuffer i) override { m_latencyRequirement = i.ReadDouble(); }
    void Print(std::ostream &os) const override { os << "Latency=" << m_latencyRequirement; }
    void SetLatency(double latency) { m_latencyRequirement = latency; }
    double GetLatency() const { return m_latencyRequirement; }

private:
    double m_latencyRequirement = 0.0;
};

class VirtualSlice
{
public:
    VirtualSlice(std::string name, double bandwidth, uint32_t maxQueue)
        : name(name), bandwidthMbps(bandwidth), maxQueueBytes(maxQueue), packetsDropped(0), packetsEnqueued(0),
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
        throughputActual = totalBytes * 8.0 / (Simulator::Now().GetSeconds() + 1e-6) / 1e6; // Mbps
        return true;
    }
    
    void DropPolicy() {}
    
    void Resize(double newBw, uint32_t newQueueSize)
    {
        bandwidthMbps = newBw;
        maxQueueBytes = newQueueSize;
        NS_LOG_INFO("Slice " << name << " resized: BW=" << bandwidthMbps << " Mbps, Queue=" << maxQueueBytes << " bytes");
        csvFile << Simulator::Now().GetSeconds() << ",SLICE_RESIZE,-1,-1,-1," << name << ",-1,-1," << bandwidthMbps << "," << GetQueueOccupancy() << "\n";
    }
    
    double GetQueueOccupancy() const
    {
        return buffer.size() * 1000.0 / maxQueueBytes;
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
    double packetRate; // packets/s
    double throughputActual; // Mbps
};

class PacketGeneratorApp : public Application
{
public:
    PacketGeneratorApp() : m_socket(0), m_running(false), m_ueId(0), m_sliceType(eMBB), m_lastPacketTime(0.0) {}
    virtual ~PacketGeneratorApp() { m_socket = 0; }
    
    void Setup(Address peer, uint32_t ueId, SliceType slice)
    {
        m_peer = peer;
        m_ueId = ueId;
        m_sliceType = slice;
    }

private:
    void StartApplication(void) override
    {
        m_running = true;
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Connect(m_peer);
        SchedulePacket();
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
        if (!m_running || !m_isOnline) return;
        
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
        else { minLatency = 0.1; maxLatency = 10.0; }
        latencyRv->SetAttribute("Min", DoubleValue(minLatency));
        latencyRv->SetAttribute("Max", DoubleValue(maxLatency));
        double latency = latencyRv->GetValue();
        
        Ptr<Packet> packet = Create<Packet>(packetSize);
        LatencyTag tag;
        tag.SetLatency(latency);
        packet->AddPacketTag(tag);
        
        double interval = 0.0;
        if (m_socket->Send(packet) >= 0)
        {
            double now = Simulator::Now().GetSeconds();
            if (m_lastPacketTime > 0.0)
                m_packetRate = 1.0 / (now - m_lastPacketTime);
            m_lastPacketTime = now;
            m_throughputRequired = m_packetRate * packetSize * 8.0 / 1e6; // Mbps
            NS_LOG_INFO("UE " << m_ueId << " sent packet: size=" << packetSize << ", latency=" << latency << ", rate=" << m_packetRate);
        }
        
        Ptr<ExponentialRandomVariable> intervalRv = CreateObject<ExponentialRandomVariable>();
        double meanInterval = m_sliceType == URLLC ? 0.005 : m_sliceType == eMBB ? 0.01 : 0.1;
        intervalRv->SetAttribute("Mean", DoubleValue(meanInterval));
        interval = intervalRv->GetValue();
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
        if (m_isOnline) SchedulePacket();
        ScheduleStateTransition();
    }

    Ptr<Socket> m_socket;
    Address m_peer;
    bool m_running;
    uint32_t m_ueId;
    SliceType m_sliceType;
    bool m_isOnline = true;
    EventId m_sendEvent;
    EventId m_stateEvent;
    double m_lastPacketTime;
    double m_packetRate = 0.0;
    double m_throughputRequired = 0.0; // Mbps
};

class SDNController
{
public:
    SDNController()
    {
        slices.emplace_back("eMBB", 50.0, totalQueueBytes / 3);
        slices.emplace_back("URLLC", 30.0, totalQueueBytes / 3);
        slices.emplace_back("mMTC", 20.0, totalQueueBytes / 3);
    }
    
    SliceType ClassifyPacket(Ptr<Packet> packet, double packetRate)
    {
        double size = packet->GetSize();
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        double latency = tag.GetLatency();
        
        if (latency < 0.01 && size < 500 && packetRate > 50.0) return URLLC;
        else if (size > 10000 && latency < 0.1 && packetRate > 10.0) return eMBB;
        else return mMTC;
    }
    
    bool ProcessPacket(Ptr<Packet> packet, Time arrivalTime, double packetRate, double throughputRequired)
    {
        SliceType slice = ClassifyPacket(packet, packetRate);
        bool enqueued = slices[slice].Enqueue(packet);
        
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        double latencyActual = (Simulator::Now() - arrivalTime).GetSeconds();
        slices[slice].totalLatency += latencyActual;
        slices[slice].packetRate = packetRate;
        
        csvFile << Simulator::Now().GetSeconds() << "," << packet->GetUid() << "," << packet->GetSize() << ","
                << tag.GetLatency() << "," << latencyActual << "," << packetRate << "," << throughputRequired << ","
                << slices[slice].throughputActual << "," << slices[slice].name << "," << enqueued << ","
                << (!enqueued) << "," << slices[slice].bandwidthMbps << "," << slices[slice].GetQueueOccupancy() << ","
                << slices[slice].GetQueueBytes() << "\n";
        
        return enqueued;
    }
    
    void MonitorAndAdjust()
    {
        double dropThreshold = 0.1;
        double latencyThresholds[3] = {0.1, 0.01, 10.0};
        double occupancyThreshold = 10.0; // % for releasing resources
        double resourcePoolBw = 0.0;
        uint32_t resourcePoolQueue = 0;
        
        // Step 1: Release excess resources
        for (size_t i = 0; i < slices.size(); ++i)
        {
            double occupancy = slices[i].GetQueueOccupancy();
            if (occupancy < occupancyThreshold && slices[i].bandwidthMbps > 10.0)
            {
                double releaseBw = std::min(10.0, slices[i].bandwidthMbps - 10.0);
                uint32_t releaseQueue = std::min(uint32_t(100000), slices[i].maxQueueBytes - 100000);
                slices[i].Resize(slices[i].bandwidthMbps - releaseBw, slices[i].maxQueueBytes - releaseQueue);
                resourcePoolBw += releaseBw;
                resourcePoolQueue += releaseQueue;
            }
        }
        
        // Step 2: Allocate to congested slices
        std::vector<double> demands(slices.size(), 0.0);
        double totalDemand = 0.0;
        for (size_t i = 0; i < slices.size(); ++i)
        {
            double dropRate = slices[i].GetDropRate();
            double avgLatency = slices[i].packetsEnqueued > 0 ? slices[i].totalLatency / slices[i].packetsEnqueued : 0.0;
            double occupancy = slices[i].GetQueueOccupancy();
            if (dropRate > dropThreshold || avgLatency > latencyThresholds[i] || occupancy > 80.0)
            {
                demands[i] = slices[i].packetRate * (avgLatency / latencyThresholds[i]);
                totalDemand += demands[i];
            }
        }
        
        // Step 3: Proportional allocation from pool
        if (totalDemand > 0)
        {
            for (size_t i = 0; i < slices.size(); ++i)
            {
                if (demands[i] > 0)
                {
                    double bwShare = resourcePoolBw * (demands[i] / totalDemand);
                    uint32_t queueShare = resourcePoolQueue * (demands[i] / totalDemand);
                    slices[i].Resize(slices[i].bandwidthMbps + bwShare, slices[i].maxQueueBytes + queueShare);
                }
            }
        }
        
        // Step 4: Validate total resources
        double totalBw = 0.0;
        uint32_t totalQueue = 0;
        for (const auto& slice : slices)
        {
            totalBw += slice.bandwidthMbps;
            totalQueue += slice.maxQueueBytes;
        }
        if (std::abs(totalBw - totalBandwidth) > 0.01 || totalQueue != totalQueueBytes)
            NS_LOG_WARN("Resource mismatch: BW=" << totalBw << "/" << totalBandwidth << ", Queue=" << totalQueue << "/" << totalQueueBytes);
        
        Simulator::Schedule(Seconds(monitorInterval), &SDNController::MonitorAndAdjust, this);
    }

    std::vector<VirtualSlice> slices;
};

class PacketReceiverApp : public Application
{
public:
    PacketReceiverApp(SDNController* controller) : m_socket(0), m_running(false), m_controller(controller) {}
    virtual ~PacketReceiverApp() { m_socket = 0; }
    
private:
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
            InetSocketAddress addr = InetSocketAddress::ConvertFrom(from);
            // Placeholder for packet rate and throughput; ideally passed from UE
            m_controller->ProcessPacket(packet, Simulator::Now(), 0.0, 0.0);
        }
    }

    Ptr<Socket> m_socket;
    bool m_running;
    SDNController* m_controller;
};

int main(int argc, char *argv[])
{
    uint32_t seed = 1234;
    CommandLine cmd;
    cmd.AddValue("seed", "Random seed", seed);
    cmd.Parse(argc, argv);
    generator.seed(seed);
    
    LogComponentEnable("6GNetworkSlicing", LOG_LEVEL_INFO);
    
    csvFile.open("net_slice_data.csv");
    csvFile << "Time,Packet_ID,PacketSize,LatencyReq,LatencyActual,PacketRate,ThroughputReq,ThroughputActual,Slice,Enqueued,Dropped,Slice_BW,Queue_Occupancy,Queue_Bytes\n";
    
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
    }
    
    Simulator::Schedule(Seconds(monitorInterval), &SDNController::MonitorAndAdjust, &controller);
    
    AnimationInterface anim("6g_sim.xml");
    anim.SetMaxPktsPerTraceFile(1000000);
    for (uint32_t i = 0; i < numUes; ++i)
    {
        SliceType slice = static_cast<SliceType>(i % 3);
        if (slice == eMBB) anim.UpdateNodeColor(ues.Get(i), 255, 0, 0);
        else if (slice == URLLC) anim.UpdateNodeColor(ues.Get(i), 0, 255, 0);
        else anim.UpdateNodeColor(ues.Get(i), 0, 0, 255);
    }
    for (uint32_t i = 0; i < numRouters; ++i)
    {
        SliceType slice = static_cast<SliceType>(i % 3);
        if (slice == eMBB) anim.UpdateNodeColor(routers.Get(i), 255, 0, 0);
        else if (slice == URLLC) anim.UpdateNodeColor(routers.Get(i), 0, 255, 0);
        else anim.UpdateNodeColor(routers.Get(i), 0, 0, 255);
    }
    anim.UpdateNodeColor(baseStation.Get(0), 255, 255, 255);
    anim.UpdateNodeColor(server.Get(0), 255, 255, 255);
    
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();
    
    csvFile.close();
    return 0;
}
