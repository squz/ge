// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <string>

// Run the H.264 player loop. Connects to the stream relay (spyder) at host:port and pairs with
// the server registered under `serverName`, decodes H.264 frames via
// VideoDecoder, renders via SDL, forwards input. Blocks until quit.
// Returns 0 on success, non-zero on error.
int playerCore(const std::string& host, int port,
               const std::string& serverName = "server");
