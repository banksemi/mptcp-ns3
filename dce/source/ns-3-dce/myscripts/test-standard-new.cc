#include "ns3/network-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/dce-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/netanim-module.h"
#include "ns3/constant-position-mobility-model.h"

/*
#include "ns3/wifi-module.h"
#include "ns3/lte-module.h"
#include "ns3/buildings-module.h"
#include "ns3/mmwave-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/netanim-module.h"
#include "ns3/constant-position-mobility-model.h"
#include "ns3/config-store-module.h"
*/
#include <iostream>
#include <ctime>
#include <stdlib.h>

#include <sstream>

#define SSTR( x ) static_cast< std::ostringstream & > ( \
    ( std::ostringstream () << std::dec << x ) ).str ()

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("DceMptcpTest");

std::string sched = "default";

void PrintPid (ApplicationContainer apps, DceApplicationHelper dce) {
    NS_LOG_UNCOND ("PID " << dce.GetPid (PeekPointer (apps.Get (0))));
}

void setPos (Ptr<Node> n, int x, int y, int z) {
    Ptr<ConstantPositionMobilityModel> loc = CreateObject<ConstantPositionMobilityModel> ();
    n->AggregateObject (loc);
    Vector locVec2 (x, y, z);
    loc->SetPosition (locVec2);
}


Ipv4InterfaceContainer InstallDevice(const char *DataRate, const char *Delay, Ipv4AddressHelper Address, Ptr<Node> node1, Ptr<Node> node2, int queue=0)
{    
    PointToPointHelper* pointToPoint = new PointToPointHelper();
    pointToPoint->SetDeviceAttribute ("DataRate", StringValue (DataRate));
    pointToPoint->SetChannelAttribute ("Delay", StringValue (Delay)); // 별도 옵션
    if (queue != 0) {
        pointToPoint->SetQueue ("ns3::DropTailQueue",
                            "Mode", StringValue ("QUEUE_MODE_PACKETS"),
                            "MaxPackets", UintegerValue (queue));
    }

    NetDeviceContainer devices = pointToPoint->Install (node1, node2);
    Ipv4InterfaceContainer if1 = Address.Assign (devices);




    pointToPoint->EnablePcapAll("../../pcap/" + sched);
    return if1;
}
void AddRouteForNode(int i, Ptr<Node> node, Ipv4InterfaceContainer if1, int node_index, const char* ipbase, bool is_default_gateway = false) {
    std::ostringstream cmd_oss;
    cmd_oss.str ("");
    std::pair<ns3::Ptr<ns3::Ipv4>, unsigned int> node_ipv4 =  if1.Get(node_index);

    cmd_oss << "\n* "<< i << "Node Route Table *";
    NS_LOG_UNCOND (cmd_oss.str ().c_str ());

    cmd_oss.str ("");
    cmd_oss << "rule add from " << if1.GetAddress (node_index, 0) << " table " << (node_ipv4.second + 1);
    NS_LOG_UNCOND (cmd_oss.str ().c_str ());
    LinuxStackHelper::RunIp (node, Seconds (0.1), cmd_oss.str().c_str());
    
    cmd_oss.str ("");
    cmd_oss << "route add " << ipbase << "/24 dev sim" << node_ipv4.second  << " scope link table " << (node_ipv4.second +1);
    NS_LOG_UNCOND (cmd_oss.str().c_str());
    LinuxStackHelper::RunIp (node, Seconds (0.1), cmd_oss.str().c_str());

    cmd_oss.str ("");
    cmd_oss << "route add default via " << if1.GetAddress (node_index != 1, 0) << " dev sim" << node_ipv4.second  << " table " << (node_ipv4.second +1);
    NS_LOG_UNCOND (cmd_oss.str().c_str());
    LinuxStackHelper::RunIp (node, Seconds (0.1), cmd_oss.str().c_str());

    if (is_default_gateway) {
        cmd_oss.str ("");
        cmd_oss << "route add default via " << if1.GetAddress (node_index != 1, 0) << " dev sim" << node_ipv4.second;
        NS_LOG_UNCOND (cmd_oss.str().c_str());
        LinuxStackHelper::RunIp (node, Seconds (0.1), cmd_oss.str().c_str());
    }
}
void AddRouteForRouter(int i, Ptr<Node> router, Ipv4InterfaceContainer if1, int router_index, const char* ipbase) {

    std::ostringstream cmd_oss;
    cmd_oss.str ("");
    std::pair<ns3::Ptr<ns3::Ipv4>, unsigned int> router_ipv4 =  if1.Get(router_index);

    cmd_oss << "\n* "<< i << "Router Route Table *";
    NS_LOG_UNCOND (cmd_oss.str ().c_str ());

    cmd_oss.str ("");
    // cmd_oss << "rule add from " << ipbase << "/24 via " << if1.GetAddress (router_index, 0) << " dev sim" << router_ipv4.second;
    cmd_oss << "route add " << ipbase << "/24 dev sim" << router_ipv4.second;
    NS_LOG_UNCOND (cmd_oss.str ().c_str ());
    LinuxStackHelper::RunIp (router, Seconds (0.1), cmd_oss.str().c_str());
}

int main (int argc, char *argv[]) {
    LogComponentEnable ("DceMptcpTest", LOG_LEVEL_ALL);
    uint32_t ntraffic_nodes = 4;
    CommandLine cmd;
    std::string bandwidth = "1Mbit";

    cmd.AddValue ("sched", "sched value", sched);
    cmd.AddValue ("bandwidth", "bandwidth value", bandwidth);
    cmd.Parse (argc, argv);

    NS_LOG_UNCOND ("sched " << sched);
    NS_LOG_UNCOND ("bandwidth " << bandwidth);
    
    NodeContainer nodes, routers, traffic_nodes;
    nodes.Create (2);
    routers.Create (3);
    traffic_nodes.Create (ntraffic_nodes * 2);

    DceManagerHelper dceManager;
    dceManager.SetTaskManagerAttribute ("FiberManagerType", StringValue ("UcontextFiberManager"));
    dceManager.SetNetworkStack ("ns3::LinuxSocketFdFactory", "Library", StringValue ("liblinux.so"));
    dceManager.Install (nodes);
    dceManager.Install (routers);
    dceManager.Install (traffic_nodes);

    LinuxStackHelper stack;
    stack.Install (nodes);

    LinuxStackHelper router_stack;
    stack.Install (routers);

    LinuxStackHelper traffic_nodes_stack;
    stack.Install (traffic_nodes);

    NetDeviceContainer devices;
    PointToPointHelper pointToPoint;
    Ipv4AddressHelper address[20];
    char address_base[20][100];
    Ipv4InterfaceContainer ipinterface;
    Ipv4InterfaceContainer bottleneck_ipinterface;
    std::ostringstream cmd_oss;
    for (int i = 0; i< 20; i++) {
        sprintf(address_base[i], "10.%d.0.0", i);
        address[i].SetBase (address_base[i], "255.255.255.0");
    }
    

    ipinterface = InstallDevice("100Mbps", "1ms", address[1], nodes.Get (0), routers.Get (0));
    AddRouteForNode(0, nodes.Get(0), ipinterface, 0, address_base[1], true);
    AddRouteForRouter(0, routers.Get(0),ipinterface, 1, address_base[1]);


    bottleneck_ipinterface = InstallDevice("100Mbps", "3ms", address[2], routers.Get (0), routers.Get (1), 500);
    AddRouteForRouter(0, routers.Get(0), bottleneck_ipinterface, 0, address_base[2]);
    AddRouteForRouter(0, routers.Get(0), bottleneck_ipinterface, 0, address_base[3]);
    AddRouteForRouter(1, routers.Get(1), bottleneck_ipinterface, 1, address_base[2]);
    AddRouteForRouter(1, routers.Get(1), bottleneck_ipinterface, 1, address_base[1]);

    ipinterface = InstallDevice("100Mbps", "1ms", address[3], nodes.Get (1), routers.Get (1));
    AddRouteForNode(1, nodes.Get(1), ipinterface, 0, address_base[3], true);
    AddRouteForRouter(1, routers.Get(1),ipinterface, 1, address_base[3]);

    ipinterface = InstallDevice("100Mbps", "4ms", address[4], nodes.Get (0), routers.Get (2));
    AddRouteForNode(0, nodes.Get(0), ipinterface, 0, address_base[4]);
    AddRouteForRouter(2, routers.Get(2),ipinterface, 1, address_base[4]);

    ipinterface = InstallDevice("100Mbps", "4ms", address[5], nodes.Get (1), routers.Get (2));
    AddRouteForNode(1, nodes.Get(1), ipinterface, 0, address_base[5]);
    AddRouteForRouter(2, routers.Get(2),ipinterface, 1, address_base[5]);

    int default_address_no = 6;
    for (int i = 0; i < ntraffic_nodes; i ++) {
        ipinterface = InstallDevice("100Mbps", "1ms", address[default_address_no + i * 2], traffic_nodes.Get (i * 2), routers.Get (0));
        AddRouteForNode(default_address_no + i * 2, traffic_nodes.Get(i * 2), ipinterface, 0, address_base[default_address_no + i * 2], true);
        AddRouteForRouter(0, routers.Get(0),ipinterface, 1, address_base[default_address_no + i * 2]);

        AddRouteForRouter(0, routers.Get(0), bottleneck_ipinterface, 0, address_base[default_address_no + i * 2 + 1]);
        AddRouteForRouter(1, routers.Get(1), bottleneck_ipinterface, 1, address_base[default_address_no + i * 2]);


        ipinterface = InstallDevice("100Mbps", "1ms", address[default_address_no + i * 2 + 1], traffic_nodes.Get (i * 2 + 1), routers.Get (1));
        AddRouteForNode(default_address_no + i * 2 + 1, traffic_nodes.Get(i * 2 + 1), ipinterface, 0, address_base[default_address_no + i * 2 + 1], true);
        AddRouteForRouter(1, routers.Get(1),ipinterface, 1, address_base[default_address_no + i * 2 + 1]);


    
    }

    NS_LOG_UNCOND ("bandwidth " << bandwidth);


  // default route
    //LinuxStackHelper::RunIp (nodes.Get (0), Seconds (0.1), "route add default via 10.1.0.2 dev sim0");
    //LinuxStackHelper::RunIp (nodes.Get (1), Seconds (0.1), "route add default via 10.2.0.2 dev sim0");


    stack.SysctlSet(nodes, ".net.mptcp.mptcp_enabled", "1");
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
    dce.AddArgument ("10.3.0.1");
    dce.AddArgument ("-i");
    dce.AddArgument ("1.0");
    dce.AddArgument ("--time");
    dce.AddArgument ("32");
    dce.AddArgument ("-l");
    dce.AddArgument ("4K");
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

    for (int i = 0; i < ntraffic_nodes; i ++) {
        for(float time = 7; time < 32; time += 10) {
            // iperf 클라이언트를 노드 0에 세팅
            dce.SetStackSize (1 << 20);
            dce.SetBinary ("iperf3");
            dce.ResetArguments ();
            dce.ResetEnvironment ();
            dce.AddArgument ("-c");
            cmd_oss.str ("");
            cmd_oss << "10." << (default_address_no + i * 2 + 1) << ".0.1";
            dce.AddArgument (cmd_oss.str ().c_str ());
            NS_LOG_UNCOND (cmd_oss.str ().c_str ());

            dce.AddArgument ("-i");
            dce.AddArgument ("1.0");
            dce.AddArgument ("--time");
            dce.AddArgument ("5");
            dce.AddArgument ("--bandwidth");
            dce.AddArgument ("0");
            //dce.AddArgument ("--json");
            dce.AddArgument ("-R");
            apps = dce.Install (traffic_nodes.Get(i * 2));
            apps.Start (Seconds (time));

            // iperf3 결과를 보기 위해 iperf3 pid 출력
            Simulator::Schedule (Seconds (time + 0.5), &PrintPid, apps, dce);

            // iperf 서버를 노드 1에 세팅
            dce.SetStackSize (1 << 20);
            dce.SetBinary ("iperf3");
            dce.ResetArguments ();
            dce.ResetEnvironment ();
            dce.AddArgument ("-s");
            apps = dce.Install (traffic_nodes.Get(i * 2 + 1));
            apps.Start (Seconds (time - 0.5));
        }
    }
    

    Simulator::Stop (Seconds (100));
    Simulator::Run ();
    Simulator::Destroy ();

  return 0;
}



