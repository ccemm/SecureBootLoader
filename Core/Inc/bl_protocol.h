/**
  * @file    bl_protocol.h
  * @brief   UART Firmware Update Protocol v1.0 - frame parser and command handling.
  */
#ifndef BL_PROTOCOL_H
#define BL_PROTOCOL_H

/* Reset the session state (state IDLE, no partial frame, no cached response) */
void bl_protocol_init(void);

/* Call from the main loop: parses received bytes and executes complete frames */
void bl_protocol_poll(void);

#endif /* BL_PROTOCOL_H */
