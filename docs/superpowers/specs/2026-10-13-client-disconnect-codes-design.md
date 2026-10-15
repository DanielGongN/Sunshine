# Client disconnect reason codes

The custom Moonlight client reports its reason in the 32-bit data argument of an ENet disconnect. ENet delivers the value in host byte order. Sunshine reads it only for the peer associated with the streaming session.

| ENet disconnect data | Internal reason | Outgoing disconnected.data.type |
| --- | --- | --- |
| 0x8004F001 | client_requested_stop | 5 |
| 0x8004F002 | client_displaced | 6 |
| 0 or any other value | peer_disconnected_unknown | 99 |

The client confirmed F001 means user-requested exit and F002 means displacement by another session. These are client-reported reasons. A plain disconnect cannot establish user intent. The existing explicit /cancel request uses client_requested_stop and therefore also reports 5.

Only RUNNING sessions process a new disconnect cause. Session first-cause ownership and the client_connect-cycle notification limit remain in effect. A server error or timeout already responsible for stopping a session is never replaced by a later F001/F002. Reasons include the hexadecimal ENet code in data.message and the control disconnect log.

Inbound platform commands continue to read only data.type. Existing platform types 0 through 4 and server-to-client termination codes 0x8004E000 through 0x8004E004 are unchanged. F001/F002 are client-to-server values and do not add server termination codes.

Validation uses real loopback ENet peers to verify both high-bit values survive transport, plus classification and outgoing-queue tests for types 5/6, unknown values, and first-notification ownership.
