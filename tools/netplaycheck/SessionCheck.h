#pragma once

#include "engine/net/GnsTransport.h"

void checkOnlineSession(bool host, const std::string& endpoint, const std::string& code,
                        gdl::u8 localPlayers, gdl::usize totalPlayers,
                        gdl::GnsTransport::Simulation simulation);
