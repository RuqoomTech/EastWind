/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "GameNetwork/NetPacketStructs.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/FirewallHelper.h"

// Byte sizes of everything sent over the network, taken from the win32 build.
// x64 must match so win32 and x64 players can play together; only change a number when the packet format changes on purpose.
static_assert(sizeof(NetPacketCommandTypeField) == 2, "NetPacketCommandTypeField");
static_assert(sizeof(NetPacketRelayField) == 2, "NetPacketRelayField");
static_assert(sizeof(NetPacketFrameField) == 5, "NetPacketFrameField");
static_assert(sizeof(NetPacketPlayerIdField) == 2, "NetPacketPlayerIdField");
static_assert(sizeof(NetPacketCommandIdField) == 3, "NetPacketCommandIdField");
static_assert(sizeof(NetPacketDataField) == 1, "NetPacketDataField");
static_assert(sizeof(NetPacketRepeatField) == 1, "NetPacketRepeatField");
static_assert(sizeof(NetPacketRepeatCommand::CommandBase) == 1, "NetPacketRepeatCommand::CommandBase");
static_assert(sizeof(SmallNetPacketCommandBase::CommandBase) == 15, "SmallNetPacketCommandBase::CommandBase");
static_assert(sizeof(NetPacketAckCommandData::FixedData) == 3, "NetPacketAckCommandData::FixedData");
static_assert(sizeof(NetPacketAckCommandBase::CommandBase) == 5, "NetPacketAckCommandBase::CommandBase");
static_assert(sizeof(NetPacketFrameCommandData::FixedData) == 2, "NetPacketFrameCommandData::FixedData");
static_assert(sizeof(NetPacketFrameCommandBase::CommandBase) == 15, "NetPacketFrameCommandBase::CommandBase");
static_assert(sizeof(NetPacketPlayerLeaveCommandData::FixedData) == 1, "NetPacketPlayerLeaveCommandData::FixedData");
static_assert(sizeof(NetPacketPlayerLeaveCommandBase::CommandBase) == 15, "NetPacketPlayerLeaveCommandBase::CommandBase");
static_assert(sizeof(NetPacketRunAheadMetricsCommandData::FixedData) == 6, "NetPacketRunAheadMetricsCommandData::FixedData");
static_assert(sizeof(NetPacketRunAheadMetricsCommandBase::CommandBase) == 10, "NetPacketRunAheadMetricsCommandBase::CommandBase");
static_assert(sizeof(NetPacketRunAheadCommandData::FixedData) == 3, "NetPacketRunAheadCommandData::FixedData");
static_assert(sizeof(NetPacketRunAheadCommandBase::CommandBase) == 15, "NetPacketRunAheadCommandBase::CommandBase");
static_assert(sizeof(NetPacketDestroyPlayerCommandData::FixedData) == 4, "NetPacketDestroyPlayerCommandData::FixedData");
static_assert(sizeof(NetPacketDestroyPlayerCommandBase::CommandBase) == 15, "NetPacketDestroyPlayerCommandBase::CommandBase");
static_assert(sizeof(NetPacketKeepAliveCommandBase::CommandBase) == 7, "NetPacketKeepAliveCommandBase::CommandBase");
static_assert(sizeof(NetPacketDisconnectKeepAliveCommandBase::CommandBase) == 7, "NetPacketDisconnectKeepAliveCommandBase::CommandBase");
static_assert(sizeof(NetPacketDisconnectPlayerCommandData::FixedData) == 5, "NetPacketDisconnectPlayerCommandData::FixedData");
static_assert(sizeof(NetPacketDisconnectPlayerCommandBase::CommandBase) == 10, "NetPacketDisconnectPlayerCommandBase::CommandBase");
static_assert(sizeof(NetPacketRouterQueryCommandBase::CommandBase) == 7, "NetPacketRouterQueryCommandBase::CommandBase");
static_assert(sizeof(NetPacketRouterAckCommandBase::CommandBase) == 7, "NetPacketRouterAckCommandBase::CommandBase");
static_assert(sizeof(NetPacketDisconnectVoteCommandData::FixedData) == 5, "NetPacketDisconnectVoteCommandData::FixedData");
static_assert(sizeof(NetPacketDisconnectVoteCommandBase::CommandBase) == 10, "NetPacketDisconnectVoteCommandBase::CommandBase");
static_assert(sizeof(NetPacketChatCommandBase::CommandBase) == 15, "NetPacketChatCommandBase::CommandBase");
static_assert(sizeof(NetPacketDisconnectChatCommandBase::CommandBase) == 7, "NetPacketDisconnectChatCommandBase::CommandBase");
static_assert(sizeof(NetPacketGameCommandBase::CommandBase) == 15, "NetPacketGameCommandBase::CommandBase");
static_assert(sizeof(NetPacketWrapperCommandData::FixedData) == 22, "NetPacketWrapperCommandData::FixedData");
static_assert(sizeof(NetPacketWrapperCommandBase::CommandBase) == 10, "NetPacketWrapperCommandBase::CommandBase");
static_assert(sizeof(NetPacketFileCommandBase::CommandBase) == 10, "NetPacketFileCommandBase::CommandBase");
static_assert(sizeof(NetPacketFileAnnounceCommandBase::CommandBase) == 10, "NetPacketFileAnnounceCommandBase::CommandBase");
static_assert(sizeof(NetPacketFileProgressCommandData::FixedData) == 6, "NetPacketFileProgressCommandData::FixedData");
static_assert(sizeof(NetPacketFileProgressCommandBase::CommandBase) == 10, "NetPacketFileProgressCommandBase::CommandBase");
static_assert(sizeof(NetPacketProgressCommandData::FixedData) == 1, "NetPacketProgressCommandData::FixedData");
static_assert(sizeof(NetPacketProgressCommandBase::CommandBase) == 7, "NetPacketProgressCommandBase::CommandBase");
static_assert(sizeof(NetPacketLoadCompleteCommandBase::CommandBase) == 10, "NetPacketLoadCompleteCommandBase::CommandBase");
static_assert(sizeof(NetPacketTimeOutGameStartCommandBase::CommandBase) == 10, "NetPacketTimeOutGameStartCommandBase::CommandBase");
static_assert(sizeof(NetPacketDisconnectFrameCommandData::FixedData) == 4, "NetPacketDisconnectFrameCommandData::FixedData");
static_assert(sizeof(NetPacketDisconnectFrameCommandBase::CommandBase) == 10, "NetPacketDisconnectFrameCommandBase::CommandBase");
static_assert(sizeof(NetPacketDisconnectScreenOffCommandData::FixedData) == 4, "NetPacketDisconnectScreenOffCommandData::FixedData");
static_assert(sizeof(NetPacketDisconnectScreenOffCommandBase::CommandBase) == 10, "NetPacketDisconnectScreenOffCommandBase::CommandBase");
static_assert(sizeof(NetPacketFrameResendRequestCommandData::FixedData) == 4, "NetPacketFrameResendRequestCommandData::FixedData");
static_assert(sizeof(NetPacketFrameResendRequestCommandBase::CommandBase) == 10, "NetPacketFrameResendRequestCommandBase::CommandBase");
static_assert(sizeof(TransportMessageHeader) == 6, "TransportMessageHeader");
static_assert(sizeof(TransportMessage) == 1040, "TransportMessage");
static_assert(sizeof(LANMessage) == 471, "LANMessage");
static_assert(sizeof(ManglerData) == 20, "ManglerData");
static_assert(sizeof(ManglerMessage) == 30, "ManglerMessage");
