/*
 * Boot button and setup hotspot: the decisions (see setupap.h).
 * No Arduino calls in here, so the host tests run this file as it is.
 */

#include "setupap.h"

uint8_t bootbutton_update(struct bootbutton_t *b, bool pressed, unsigned long now) {
  if(!b->armed && (unsigned long)(now - b->last_short) >= BOOTBUTTON_REARM_MS) {
    b->armed = 1;
  }
  if(pressed) {
    if(!b->down) {
      b->down = 1;
      b->fired = 0;
      b->held = 0;
    } else {
      unsigned long step = (unsigned long)(now - b->last);
      if(step <= BOOTBUTTON_GAP_MS) {  // a longer gap: nobody watched the pin, it does not count
        b->held += step;
      }
    }
    b->last = now;
    if(!b->fired && b->held > BOOTBUTTON_FACTORY_RESET_MS) {
      b->fired = 1;
      return BOOTBUTTON_FACTORY_RESET;
    }
    return BOOTBUTTON_NONE;
  }
  if(!b->down) {
    return BOOTBUTTON_NONE;
  }
  b->down = 0;
  // the release: only when it was seen right after the last "pressed" poll, the length is known
  if(b->fired || (unsigned long)(now - b->last) > BOOTBUTTON_GAP_MS) {
    return BOOTBUTTON_NONE;
  }
  if(b->held < BOOTBUTTON_SHORT_MIN_MS || b->held > BOOTBUTTON_SHORT_MAX_MS || !b->armed) {
    return BOOTBUTTON_NONE;
  }
  b->armed = 0;
  b->last_short = now;
  return BOOTBUTTON_SHORT;
}

static void request_start(struct setupap_t *s, unsigned long now) {
  s->requested = 1;
  s->request_time = now;
  s->seen_time = now;
}

uint8_t setupap_button(struct setupap_t *s, const struct setupap_input_t *in, unsigned long now) {
  if(in->net_up) {
    s->requested = 0;
    return SETUPAP_EVENT_IGNORED;  // the device can be reached through its network
  }
  if(s->requested) {
    s->requested = 0;
    return SETUPAP_EVENT_CANCELLED;
  }
  request_start(s, now);
  return SETUPAP_EVENT_REQUESTED;
}

uint8_t setupap_update(struct setupap_t *s, const struct setupap_input_t *in, unsigned long now) {
  uint8_t before = s->reason;
  s->event = SETUPAP_EVENT_NONE;
  if(in->net_up) {
    if(s->requested) {
      s->requested = 0;
      s->event = SETUPAP_EVENT_NETWORK;
    }
    s->reason = SETUPAP_OFF;
    return s->reason;
  }
  if(s->requested) {
    if(in->clients > 0) {
      s->seen_time = now;
    }
    if((unsigned long)(now - s->request_time) >= SETUPAP_MAX_MS || (unsigned long)(now - s->seen_time) >= SETUPAP_IDLE_MS) {
      s->requested = 0;
      s->event = SETUPAP_EVENT_TIMEOUT;
    }
  }
  if(!in->wifi_configured && !in->eth_seen && in->allow_factory) {
    s->reason = SETUPAP_FACTORY;
  } else if(in->auto_on_loss) {
    s->reason = SETUPAP_AUTO;
  } else {
    if(before == SETUPAP_FACTORY && in->wifi_configured && !s->requested) {
      // a WiFi name was saved on the factory hotspot: when that WiFi cannot be reached
      // (wrong password), the hotspot has to come back, or nobody can correct it
      request_start(s, now);
      s->event = SETUPAP_EVENT_KEPT;
    }
    s->reason = s->requested ? SETUPAP_BUTTON : SETUPAP_OFF;
  }
  return s->reason;
}
