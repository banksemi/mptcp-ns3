#include "ns3/network-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/dce-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/netanim-module.h"
#include "ns3/constant-position-mobility-model.h"


#include "ns3/network-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/dce-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/netanim-module.h"
#include "ns3/constant-position-mobility-model.h"
#include "ns3/config-store-module.h"

#include <iostream>
#include <ctime>
#include <stdlib.h>

#include <sstream>

#define SSTR( x ) static_cast< std::ostringstream & > ( \
    ( std::ostringstream () << std::dec << x ) ).str ()

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("DceMptcpTest");

void PrintPid (ApplicationContainer apps, DceApplicationHelper dce) {
    NS_LOG_UNCOND ("PID " << dce.GetPid (PeekPointer (apps.Get (0))));
}
void ChangeRTT(int device, StringValue BandRate, StringValue RTT) {
    // /NodeList/0/DeviceList/0/: 노드0 ->*path1* -> 라우터 -> ... -> 노드1
    // /NodeList/0/DeviceList/1/: 노드0 ->*path2* -> 라우터 -> ... -> 노드1

    // /ChannelList/0: 노드0 -> *path1* -> 라우터 -> ... -> 노드1
    // /ChannelList/2: 노드0 -> *path2* -> 라우터 -> ... -> 노드1
    
    // /ChannelList/1: 노드0 -> ... -> 라우터 -> *path1* -> 노드1
    // /ChannelList/3: 노드0 -> ... -> 라우터 -> *path2* -> 노드1

    if (device == 0) {    
        // 0
        Config::Set("/NodeList/0/DeviceList/0/$ns3::PointToPointNetDevice/DataRate", BandRate);
        Config::Set("/NodeList/2/DeviceList/0/$ns3::PointToPointNetDevice/DataRate", BandRate);
        Config::Set("/ChannelList/1/$ns3::PointToPointChannel/Delay", RTT);
	} else {
       // Config::Set("/NodeList/0/DeviceList/1/$ns3::PointToPointNetDevice/DataRate", BandRate);
        //Config::Set("/ChannelList/2/$ns3::PointToPointChannel/Delay", RTT);
	}
}
void setPos (Ptr<Node> n, int x, int y, int z) {
    Ptr<ConstantPositionMobilityModel> loc = CreateObject<ConstantPositionMobilityModel> ();
    n->AggregateObject (loc);
    Vector locVec2 (x, y, z);
    loc->SetPosition (locVec2);
}
float Delay(float t, float a, float b, float x) {
    return t + ((x  - t + a) / a) * b - x;
}

template <typename T>
std::string to_string_with_precision(const T a_value, const int n = 6)
{
    std::ostringstream out;
    out.precision(n);
    out << std::fixed << a_value;
    return out.str();
}

int main (int argc, char *argv[]) {
    LogComponentEnable ("DceMptcpTest", LOG_LEVEL_ALL);
    uint32_t nRtrs = 1;
    CommandLine cmd;
    std::string sched = "only_fast";
    bool rtt_change = true;
    std::string bandwidth = "0";

    cmd.AddValue ("sched", "sched value", sched);
    cmd.AddValue ("rtt_change", "rtt_change value", rtt_change);
    cmd.AddValue ("bandwidth", "bandwidth value", bandwidth);
    cmd.Parse (argc, argv);

    NS_LOG_UNCOND ("sched " << sched);
    NS_LOG_UNCOND ("bandwidth " << bandwidth);
    
    NodeContainer nodes, routers;
    nodes.Create (2);
    routers.Create (nRtrs);
setPos (nodes.Get (0), 50, 0, 0);
setPos (nodes.Get (1), 50, 0, 0);
    DceManagerHelper dceManager;
    dceManager.SetTaskManagerAttribute ("FiberManagerType", StringValue ("UcontextFiberManager"));
    dceManager.SetNetworkStack ("ns3::LinuxSocketFdFactory", "Library", StringValue ("liblinux.so"));
    dceManager.Install (nodes);
    dceManager.Install (routers);

    LinuxStackHelper stack;
    stack.Install (nodes);
    stack.Install (routers);


    Ssid ssid = Ssid ("ns3-wifi");
    WifiHelper wifi = WifiHelper::Default ();
    YansWifiPhyHelper phy = YansWifiPhyHelper::Default ();
    YansWifiChannelHelper phyChannel = YansWifiChannelHelper::Default ();
    NqosWifiMacHelper mac;
    phy.Set ("Antennas", UintegerValue (2));
    phy.Set ("MaxSupportedTxSpatialStreams", UintegerValue (2));
    phy.Set ("MaxSupportedRxSpatialStreams", UintegerValue (2));
    phy.SetChannel (phyChannel.Create ());
    wifi.SetRemoteStationManager ("ns3::MinstrelHtWifiManager", "RtsCtsThreshold", UintegerValue (65535), "PrintStats", BooleanValue (true));
    mac.SetType ("ns3::AdhocWifiMac");
    wifi.SetStandard (WIFI_PHY_STANDARD_80211ac);


    PointToPointHelper pointToPoint;
    NetDeviceContainer devices1, devices2;
    Ipv4AddressHelper address1, address2;
    std::ostringstream cmd_oss;
    address1.SetBase ("10.1.0.0", "255.255.255.0");
    address2.SetBase ("10.2.0.0", "255.255.255.0");

    for (uint32_t i = 0; i < nRtrs; i++) {
        // Left 링크의 pointToPoint 속성은 DCE 스케줄에 의해 나중에 다시 세팅됨.
        // Left link
        pointToPoint.SetDeviceAttribute ("DataRate", StringValue ("1Mbps"));
        pointToPoint.SetChannelAttribute ("Delay", StringValue ("50ms")); // 별도 옵션
        devices1 = pointToPoint.Install (nodes.Get (0), routers.Get (i));
        // Assign ip addresses
        Ipv4InterfaceContainer if1 = address1.Assign (devices1);
        address1.NewNetwork ();
        // setup ip routes
        cmd_oss.str ("");
        cmd_oss << "rule add from " << if1.GetAddress (0, 0) << " table " << (i+1);
        LinuxStackHelper::RunIp (nodes.Get (0), Seconds (0.1), cmd_oss.str ().c_str ());
        cmd_oss.str ("");
        cmd_oss << "route add 10.1." << i << ".0/24 dev sim" << i << " scope link table " << (i+1);
        LinuxStackHelper::RunIp (nodes.Get (0), Seconds (0.1), cmd_oss.str ().c_str ());
        cmd_oss.str ("");
        cmd_oss << "route add default via " << if1.GetAddress (1, 0) << " dev sim" << i << " table " << (i+1);
        LinuxStackHelper::RunIp (nodes.Get (0), Seconds (0.1), cmd_oss.str ().c_str ());
        cmd_oss.str ("");
        cmd_oss << "route add 10.1."<< i <<".0/24 via " << if1.GetAddress (1, 0) << " dev sim0";
        LinuxStackHelper::RunIp (routers.Get (i), Seconds (0.2), cmd_oss.str ().c_str ());


        // Right link
        //pointToPoint.SetDeviceAttribute ("DataRate", StringValue ("10Gbps"));
        //pointToPoint.SetChannelAttribute ("Delay", StringValue ("0ns"));
        
        //devices2 = pointToPoint.Install (nodes.Get (1), routers.Get (i));
        devices2 = wifi.Install (phy, mac, NodeContainer (nodes.Get (1), routers.Get (i)));

        Ptr<WifiNetDevice> wnd = devices2.Get (0)->GetObject<WifiNetDevice> ();
        Ptr<WifiPhy> wp = wnd->GetPhy ();
        wp->SetChannelWidth (160);



        /*
        NodeContainer wifiNodes;
        wifiNodes.Add(nodes.Get (1));

        NodeContainer wifiNodes2;
        wifiNodes2.Add(routers.Get (i));

        mac.SetType ("ns3::StaWifiMac",
             "Ssid", SsidValue (ssid));

        apdevices = wifi.Install (phy, mac, wifiNodes);


        mac.SetType ("ns3::ApWifiMac",
             "Ssid", SsidValue (ssid));

        stadevices = wifi.Install (phy, mac, wifiNodes2);


        Ptr<NetDevice> ndClient = apdevices.Get (0);
        Ptr<NetDevice> ndServer = stadevices.Get (0);
        Ptr<WifiNetDevice> wndClient = ndClient->GetObject<WifiNetDevice> ();
        Ptr<WifiNetDevice> wndServer = ndServer->GetObject<WifiNetDevice> ();
        Ptr<WifiPhy> wifiPhyPtrClient = wndClient->GetPhy ();
        Ptr<WifiPhy> wifiPhyPtrServer = wndServer->GetPhy ();


        wifiPhyPtrClient->SetChannelWidth (40);
        wifiPhyPtrServer->SetChannelWidth (40);
        */

        /*
        // Assign ip addresses
        
        */
        // setup ip routes
        cmd_oss.str ("");
        Ipv4InterfaceContainer if2 = address2.Assign (devices2);
        address2.NewNetwork ();
        Ipv4Address ip1 =  if2.GetAddress (0, 0) ; // apdevices.Get (0)->GetAddress (); // 
        
        Ipv4Address ip2 = if2.GetAddress (1, 0) ;// stadevices.Get (0)->GetAddress (); // 



        cmd_oss << "rule add from " << if2.GetAddress (0, 0) << " table " << (i+1);
        LinuxStackHelper::RunIp (nodes.Get (1), Seconds (0.1), cmd_oss.str ().c_str ());
        cmd_oss.str ("");
        cmd_oss << "route add 10.2." << i << ".0/24 dev sim" << i << " scope link table " << (i+1);
        LinuxStackHelper::RunIp (nodes.Get (1), Seconds (0.1), cmd_oss.str ().c_str ());
        cmd_oss.str ("");
        cmd_oss << "route add default via " << if2.GetAddress (1, 0) << " dev sim" << i << " table " << (i+1);
        LinuxStackHelper::RunIp (nodes.Get (1), Seconds (0.1), cmd_oss.str ().c_str ());
        cmd_oss.str ("");
        cmd_oss << "route add 10.2."<< i <<".0/24 via " << if2.GetAddress (1, 0) << " dev sim1";
        LinuxStackHelper::RunIp (routers.Get (i), Seconds (0.2), cmd_oss.str ().c_str ());

        setPos (routers.Get (i), 50, 35, 0);
    }

  // default route
    LinuxStackHelper::RunIp (nodes.Get (0), Seconds (0.1), "route add default via 10.1.0.2 dev sim0");
    LinuxStackHelper::RunIp (nodes.Get (1), Seconds (0.1), "route add default via 10.2.0.2 dev sim0");
    LinuxStackHelper::RunIp (nodes.Get (0), Seconds (0.1), "rule show");


    stack.SysctlSet(nodes, ".net.mptcp.mptcp_enabled", "0");
    stack.SysctlSet(nodes, ".net.mptcp.mptcp_scheduler", sched);
    stack.SysctlSet(nodes, ".net.mptcp.mptcp_checksum", "1");
    
    DceApplicationHelper dce;
    ApplicationContainer apps;

    // iperf 클라이언트를 노드 0에 세팅
    dce.SetStackSize (1 << 20);
    dce.SetBinary ("iperf3");
    dce.ResetArguments ();
    dce.ResetEnvironment ();
    dce.AddArgument ("-c");
    dce.AddArgument ("10.2.0.1");
    dce.AddArgument ("-i");
    dce.AddArgument ("1.0");
    dce.AddArgument ("--time");
    dce.AddArgument ("10");
    dce.AddArgument ("--bandwidth");
    dce.AddArgument (bandwidth);
    //dce.AddArgument ("--json");
    dce.AddArgument ("-R");
    apps = dce.Install (nodes.Get (0));
    apps.Start (Seconds (2.0));

    // iperf3 결과를 보기 위해 iperf3 pid 출력
    Simulator::Schedule (Seconds (2.1), &PrintPid, apps, dce);

    // iperf 서버를 노드 1에 세팅
    dce.SetStackSize (1 << 20);
    dce.SetBinary ("iperf3");
    dce.ResetArguments ();
    dce.ResetEnvironment ();
    dce.AddArgument ("-s");
    apps = dce.Install (nodes.Get (1));
    apps.Start (Seconds (1.5));

    StringValue set_bandwidth1 = StringValue("100Mbps");
    int set_rtt1 = 5;

    StringValue set_bandwidth2 = StringValue("100Mbps");
    int set_rtt2 = 5;
    int _switch = 0;
    bool pacing = true;

    Simulator::Schedule(Seconds(1), &ChangeRTT, (_switch) % 2 , set_bandwidth1, StringValue(std::to_string(set_rtt1) + "ms")); // start UE movement
    Simulator::Schedule(Seconds(1), &ChangeRTT, (_switch + 1) % 2, set_bandwidth2, StringValue(std::to_string(set_rtt2) + "ms")); // start UE movement
    _switch++;

    if (rtt_change) {
        for(int i = 7; i<32; i += 5) {
            if (pacing == false) {
                Simulator::Schedule (Seconds (i), &ChangeRTT, (_switch) % 2, set_bandwidth1, StringValue(std::to_string(set_rtt1) + "ms")); // start UE movement
                Simulator::Schedule (Seconds (i), &ChangeRTT, (_switch + 1) % 2, set_bandwidth2, StringValue(std::to_string(set_rtt2) + "ms")); // start UE movement
            } else {
                int t = i * 1000;
                for (int x = t - set_rtt1; x <= t; x++) {
                    StringValue delay_ms = StringValue(to_string_with_precision<float>(Delay(t, set_rtt1, set_rtt2, x)) + "ms");
                    Simulator::Schedule (Seconds (x / 1000.0), &ChangeRTT, (_switch + 1) % 2, set_bandwidth2, delay_ms); // start UE movement
                }
                for (int x = t - set_rtt2; x <= t; x++) {
                    StringValue delay_ms = StringValue(to_string_with_precision<float>(Delay(t, set_rtt2, set_rtt1, x)) + "ms");
                    Simulator::Schedule (Seconds (x / 1000.0), &ChangeRTT, (_switch) % 2, set_bandwidth1, delay_ms); // start UE movement
                }
            }
            _switch++;
	    }
    }

    pointToPoint.EnablePcapAll ("../../pcap/pcap_file");
    

    Simulator::Stop (Seconds (100));
    Simulator::Run ();
    Simulator::Destroy ();

  return 0;
}



