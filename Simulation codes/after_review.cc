#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
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
const double simTime = 20.0;
const double monitorInterval = 0.5;
const double totalBandwidth = 100.0;
const uint32_t maxQueueBytes = 100000;
const double areaSize = 50.0;

std::ofstream csvFile;
uint32_t globalSeed = 1234; // Configurable via command-line

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
    uint32_t GetSerializedSize(void) const override { return 16; }
    void Serialize(TagBuffer i) const override
    {
        i.WriteDouble(m_latencyRequirement);
        i.WriteDouble(m_creationTime);
    }
    void Deserialize(TagBuffer i) override
    {
        m_latencyRequirement = i.ReadDouble();
        m_creationTime = i.ReadDouble();
    }
    void Print(std::ostream &os) const override
    {
        os << "LatencyReq=" << m_latencyRequirement << ", CreationTime=" << m_creationTime;
    }
    void SetLatency(double latency) { m_latencyRequirement = latency; }
    double GetLatency() const { return m_latencyRequirement; }
    void SetCreationTime(double time) { m_creationTime = time; }
    double GetCreationTime() const { return m_creationTime; }

private:
    double m_latencyRequirement = 0.0;
    double m_creationTime = 0.0;
};

class VirtualSlice
{
public:
    VirtualSlice(std::string name, double bandwidth, uint32_t maxQueue)
        : name(name), bandwidthMbps(bandwidth), maxQueueBytes(maxQueue), packetsDropped(0), packetsEnqueued(0),
          packetsTransmitted(0), totalLatency(0.0), totalQueueDelay(0.0), totalBytes(0.0), throughput(0.0)
    {
        ScheduleDequeue();
    }
    
    bool Enqueue(Ptr<Packet> packet, Time arrivalTime)
    {
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        SliceType slice = ClassifyPacket(packet);
        
        if (buffer.size() * packet->GetSize() >= maxQueueBytes)
        {
            DropPolicy(packet, slice);
            packetsDropped++;
            NS_LOG_INFO("Slice " << name << ": Packet dropped (queue full)");
            return false;
        }
        buffer.push(std::make_pair(packet, arrivalTime));
        packetsEnqueued++;
        totalBytes += packet->GetSize();
        return true;
    }
    
    void DropPolicy(Ptr<Packet> packet, SliceType slice)
    {
        // Priority-based drop: Keep URLLC packets if possible
        if (slice == URLLC && !buffer.empty())
        {
            auto front = buffer.front();
            LatencyTag tag;
            front.first->PeekPacketTag(tag);
            if (ClassifyPacket(front.first) != URLLC)
            {
                buffer.pop();
                packetsDropped++;
                NS_LOG_INFO("Slice " << name << ": Dropped non-URLLC packet to prioritize URLLC");
            }
        }
    }
    
    void Dequeue()
    {
        if (buffer.empty()) return;
        
        double timeSlot = monitorInterval; // Seconds
        double bytesPerSlot = (bandwidthMbps * 1e6 / 8) * timeSlot; // Bytes
        double bytesSent = 0.0;
        
        while (!buffer.empty() && bytesSent < bytesPerSlot)
        {
            auto item = buffer.front();
            Ptr<Packet> packet = item.first;
            Time arrivalTime = item.second;
            
            double packetSize = packet->GetSize();
            if (bytesSent + packetSize > bytesPerSlot) break;
            
            buffer.pop();
            bytesSent += packetSize;
            packetsTransmitted++;
            
            Time now = Simulator::Now();
            double queueDelay = (now - arrivalTime).GetSeconds();
            totalQueueDelay += queueDelay;
            
            LatencyTag tag;
            packet->PeekPacketTag(tag);
            double totalLatency = (now.GetSeconds() - tag.GetCreationTime());
            totalLatency += totalLatency;
            
            throughput += packetSize * 8.0 / timeSlot / 1e6; // Mbps
        }
        
        Simulator::Schedule(Seconds(timeSlot), &VirtualSlice::Dequeue, this);
    }
    
    void ScheduleDequeue()
    {
        Simulator::Schedule(Seconds(monitorInterval), &VirtualSlice::Dequeue, this);
    }
    
    void Resize(double newBw, uint32_t newQueueSize)
    {
        bandwidthMbps = newBw;
        maxQueueBytes = newQueueSize;
        NS_LOG_INFO("Slice " << name << " resized: BW=" << bandwidthMbps << " Mbps, Queue=" << maxQueueBytes << " bytes");
    }
    
    double GetQueueOccupancy() const
    {
        return buffer.size() * 1000.0 / maxQueueBytes;
    }
    
    double GetDropRate() const
    {
        return packetsEnqueued > 0 ? packetsDropped / static_cast<double>(packetsEnqueued + packetsDropped) : 0.0;
    }
    
    SliceType ClassifyPacket(Ptr<Packet> packet)
    {
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        double latency = tag.GetLatency();
        double size = packet->GetSize();
        
        if (latency < 0.001 && size < 500) return URLLC;
        else if (size > 10000 && latency < 1.0) return eMBB;
        else return mMTC;
    }

    std::string name;
    double bandwidthMbps;
    uint32_t maxQueueBytes;
    std::queue<std::pair<Ptr<Packet>, Time>> buffer; // Store packet and arrival time
    uint32_t packetsDropped;
    uint32_t packetsEnqueued;
    uint32_t packetsTransmitted;
    double totalLatency;
    double totalQueueDelay;
    double totalBytes;
    double throughput;
};

class PacketGeneratorApp : public Application
{
public:
    PacketGeneratorApp() : m_socket(0), m_running(false), m_ueId(0), m_sliceType(eMBB) {}
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
        if (m_socket->Connect(m_peer) < 0)
        {
            NS_LOG_ERROR("UE " << m_ueId << ": Socket connection failed");
            return;
        }
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
        Ptr<UniformRandomVariable> latencyRv = CreateObject<UniformRandomVariable>();
        
        // Realistic packet sizes and latency requirements
        if (m_sliceType == URLLC)
        {
            sizeRv->SetAttribute("Min", DoubleValue(50));
            sizeRv->SetAttribute("Max", DoubleValue(500));
            latencyRv->SetAttribute("Min", DoubleValue(0.0001)); // 0.1 ms
            latencyRv->SetAttribute("Max", DoubleValue(0.001));  // 1 ms
        }
        else if (m_sliceType == eMBB)
        {
            sizeRv->SetAttribute("Min", DoubleValue(10000));
            sizeRv->SetAttribute("Max", DoubleValue(100000));
            latencyRv->SetAttribute("Min", DoubleValue(0.01)); // 10 ms
            latencyRv->SetAttribute("Max", DoubleValue(1.0));  // 1 s
        }
        else // mMTC
        {
            sizeRv->SetAttribute("Min", DoubleValue(100));
            sizeRv->SetAttribute("Max", DoubleValue(1000));
            latencyRv->SetAttribute("Min", DoubleValue(0.1)); // 100 ms
            latencyRv->SetAttribute("Max", DoubleValue(10.0)); // 10 s
        }
        
        uint32_t packetSize = static_cast<uint32_t>(sizeRv->GetValue());
        double latency = latencyRv->GetValue();
        
        Ptr<Packet> packet = Create<Packet>(packetSize);
        LatencyTag tag;
        tag.SetLatency(latency);
        tag.SetCreationTime(Simulator::Now().GetSeconds());
        packet->AddPacketTag(tag);
        
        if (m_socket->Send(packet) >= 0)
        {
            NS_LOG_INFO("UE " << m_ueId << " sent packet: size=" << packetSize << ", latencyReq=" << latency);
        }
        else
        {
            NS_LOG_ERROR("UE " << m_ueId << ": Packet send failed");
        }
        
        Ptr<ExponentialRandomVariable> intervalRv = CreateObject<ExponentialRandomVariable>();
        intervalRv->SetAttribute("Mean", DoubleValue(m_sliceType == URLLC ? 0.005 : m_sliceType == eMBB ? 0.05 : 0.5));
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
};

class SDNController
{
public:
    SDNController()
    {
        slices.emplace_back("eMBB", 50.0, maxQueueBytes);
        slices.emplace_back("URLLC", 30.0, maxQueueBytes);
        slices.emplace_back("mMTC", 20.0, maxQueueBytes);
    }
    
    SliceType ClassifyPacket(Ptr<Packet> packet)
    {
        double size = packet->GetSize();
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        double latency = tag.GetLatency();
        
        if (latency < 0.001 && size < 500) return URLLC;
        else if (size > 10000 && latency < 1.0) return eMBB;
        else return mMTC;
    }
    
    bool ProcessPacket(Ptr<Packet> packet, Time arrivalTime)
    {
        SliceType slice = ClassifyPacket(packet);
        bool enqueued = slices[slice].Enqueue(packet, arrivalTime);
        
        LatencyTag tag;
        packet->PeekPacketTag(tag);
        double actualLatency = (Simulator::Now().GetSeconds() - tag.GetCreationTime());
        double queueDelay = enqueued ? (Simulator::Now() - arrivalTime).GetSeconds() : 0.0;
        
        csvFile << Simulator::Now().GetSeconds() << "," << packet->GetUid() << "," << packet->GetSize() << ","
                << tag.GetLatency() << "," << actualLatency << "," << queueDelay << "," << slices[slice].name << ","
                << enqueued << "," << (!enqueued) << "," << slices[slice].bandwidthMbps << ","
                << slices[slice].GetQueueOccupancy() << "," << slices[slice].throughput << "\n";
        
        return enqueued;
    }
    
    double CalculateFairness()
    {
        double sumBw = 0.0, sumBwSquared = 0.0;
        for (const auto& slice : slices)
        {
            sumBw += slice.bandwidthMbps;
            sumBwSquared += slice.bandwidthMbps * slice.bandwidthMbps;
        }
        return sumBw * sumBw / (slices.size() * sumBwSquared);
    }
    
    void MonitorAndAdjust()
    {
        double dropThreshold = 0.1;
        double latencyThresholds[3] = {1.0, 0.001, 10.0}; // eMBB, URLLC, mMTC
        
        for (size_t i = 0; i < slices.size(); ++i)
        {
            double dropRate = slices[i].GetDropRate();
            double avgLatency = slices[i].packetsEnqueued > 0 ? slices[i].totalLatency / slices[i].packetsEnqueued : 0.0;
            
            if (dropRate > dropThreshold || avgLatency > latencyThresholds[i])
            {
                double extraBw = 10.0;
                size_t donor = (i + 1) % slices.size();
                
                // Ensure total bandwidth constraint
                double totalBw = 0.0;
                for (const auto& slice : slices) totalBw += slice.bandwidthMbps;
                if (totalBw + extraBw <= totalBandwidth && slices[donor].bandwidthMbps > extraBw)
                {
                    slices[i].Resize(slices[i].bandwidthMbps + extraBw, slices[i].maxQueueBytes);
                    slices[donor].Resize(slices[donor].bandwidthMbps - extraBw, slices[donor].maxQueueBytes);
                    NS_LOG_INFO("Reallocated " << extraBw << " Mbps from " << slices[donor].name << " to " << slices[i].name);
                }
            }
        }
        
        double fairness = CalculateFairness();
        NS_LOG_INFO("Jain's Fairness Index: " << fairness);
        
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
        if (m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), 5000)) < 0)
        {
            NS_LOG_ERROR("PacketReceiverApp: Bind failed");
            return;
        }
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
        while ((packet = socket->Recv()))
        {
            m_controller->ProcessPacket(packet, Simulator::Now());
        }
    }

    Ptr<Socket> m_socket;
    bool m_running;
    SDNController* m_controller;
};

int main(int argc, char *argv[])
{
    CommandLine cmd;
    cmd.AddValue("seed", "Random seed for simulation", globalSeed);
    cmd.Parse(argc, argv);
    
    SeedManager::SetSeed(globalSeed);
    
    LogComponentEnable("6GNetworkSlicing", LOG_LEVEL_INFO);
    
    csvFile.open("network_slicing.csv");
    csvFile << "Time,Packet_ID,PacketSize,LatencyReq,ActualLatency,QueueDelay,Slice,Enqueued,Dropped,Slice_BW,Queue_Occupancy,Throughput\n";
    
    NodeContainer ues, server;
    ues.Create(numUes);
    server.Create(1);
    
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::RandomWalk2dMobilityModel",
                             "Bounds", RectangleValue(Rectangle(0, areaSize, 0, areaSize)),
                             "Speed", StringValue("ns3::UniformRandomVariable[Min=1.0|Max=5.0]"));
    mobility.Install(ues);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(server);
    
    InternetStackHelper internet;
    internet.Install(ues);
    internet.Install(server);
    
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    
    NetDeviceContainer devices;
    for (uint32_t i = 0; i < numUes; ++i)
    {
        devices.Add(p2p.Install(ues.Get(i), server.Get(0)));
    }
    
    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = ipv4.Assign(devices);
    
    SDNController controller;
    Ptr<PacketReceiverApp> receiver = CreateObject<PacketReceiverApp>(&controller);
    server.Get(0)->AddApplication(receiver);
    receiver->SetStartTime(Seconds(0.0));
    receiver->SetStopTime(Seconds(simTime));
    
    for (uint32_t i = 0; i < numUes; ++i)
    {
        SliceType slice = static_cast<SliceType>(i % 3);
        Ptr<PacketGeneratorApp> app = CreateObject<PacketGeneratorApp>();
        app->Setup(InetSocketAddress(interfaces.GetAddress(1), 5000), i, slice);
        ues.Get(i)->AddApplication(app);
        app->SetStartTime(Seconds(0.1 * i));
        app->SetStopTime(Seconds(simTime));
    }
    
    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();
    
    Simulator::Schedule(Seconds(monitorInterval), &SDNController::MonitorAndAdjust, &controller);
    
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    
    monitor->SerializeToXmlFile("flowmon-results.xml", true, true);
    
    Simulator::Destroy();
    csvFile.close();
    
    return 0;
}
