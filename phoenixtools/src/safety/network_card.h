#pragma once

/* Called by the main patch initializer; disabled mode installs no detour. */
int pt_network_card_initialize(void);

/* Player is 0 (P1) or 1 (P2); only a new press reads that player's file. */
void pt_network_card_key(unsigned int player, int pressed);

/* Consume one pending UID into the original native card-consumer frame. */
int pt_network_card_event(unsigned char *frame);
