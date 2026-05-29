#pragma once

// Line-oriented serial REPL for runtime configuration. Reads newline-
// delimited commands from Serial and dispatches them. Non-blocking — call
// serialCliTick() from the main loop.
//
// Commands:
//   help
//   wifi add <ssid> <password>
//   wifi del <ssid>
//   wifi list
//   wifi forget
//   wifi scan
//   wifi status
//   (more to come: key <sk-xxx> for Dashscope API key)

void serialCliInit();
void serialCliTick();
