#ifndef __USER_UDP_DISCOVER_H_
#define __USER_UDP_DISCOVER_H_

#include "mico.h"

/* ZControl / SmartControl_PC 局域网发现
 * PC 广播: 255.255.255.255:10182  payload {"cmd":"device report"}
 * 设备回:  单播到来源 IP:10181，JSON 含 mac/type/type_name/name
 */
#define UDP_DISCOVER_PORT   10182
#define UDP_DISCOVER_PEER_PORT 10181

void UdpDiscoverInit(void);
void UdpDiscoverDeinit(void);

#endif
