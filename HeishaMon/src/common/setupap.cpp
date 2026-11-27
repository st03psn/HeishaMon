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
  s->settling = 0;
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

void setupap_wifi_saved(struct setupap_t *s, unsigned long now) {
  request_start(s, now);
  s->settling = 1;
}

uint8_t setupap_update(struct setupap_t *s, const struct setupap_input_t *in, unsigned long now) {
  uint8_t before = s->reason;
  s->event = SETUPAP_EVENT_NONE;
  if(s->settling && (unsigned long)(now - s->request_time) >= SETUPAP_SETTLE_MS) {
    s->settling = 0;
  }
  if(in->net_up) {
    // right after new WiFi data was saved the old connection can still look up: that does not end the request
    if(s->requested && !s->settling) {
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

void wifiretry_started(struct wifiretry_t *r, unsigned long now) {
  r->last = now;
  r->sta_off = 0;
}

uint8_t wifiretry_net_up(struct wifiretry_t *r, const struct wifiretry_input_t *in, unsigned long now) {
  r->last = now;
  if(r->sta_off) {
    r->sta_off = 0;
    if(in->wifi_configured) {
      return WIFIRETRY_BEGIN;  // or the WiFi would stay off for as long as the ethernet is up
    }
  }
  return WIFIRETRY_NONE;
}

uint8_t wifiretry_update(struct wifiretry_t *r, const struct wifiretry_input_t *in, unsigned long now) {
  unsigned long since = (unsigned long)(now - r->last);
  if(in->scanning) {
    return WIFIRETRY_NONE;  // a step now would end the scan without a result
  }
  if(in->ap_up) {
    if(in->clients > 0) {
      // somebody uses the hotspot: the station must not hop through the channels
      if(!r->sta_off) {
        r->sta_off = 1;
        return WIFIRETRY_STA_OFF;
      }
      return WIFIRETRY_NONE;
    }
    if(since < WIFIRETRY_AP_MS) {
      return WIFIRETRY_NONE;
    }
    r->last = now;
    if(!r->sta_off) {
      r->sta_off = 1;
      return WIFIRETRY_STA_OFF;
    }
    if(!in->wifi_configured) {
      return WIFIRETRY_NONE;  // nothing to connect to: the station stays off
    }
    r->sta_off = 0;
    return WIFIRETRY_BEGIN;
  }
  // no hotspot: the station is never switched off
  if(!in->wifi_configured) {
    return WIFIRETRY_NONE;
  }
  if(r->sta_off) {
    // the hotspot went away during a "station off" step: begin again now
    r->sta_off = 0;
    r->last = now;
    return WIFIRETRY_BEGIN;
  }
  if(since < WIFIRETRY_MS) {
    return WIFIRETRY_NONE;
  }
  if(in->busy && since < WIFIRETRY_MS + WIFIRETRY_BUSY_MAX_MS) {
    return WIFIRETRY_NONE;
  }
  r->last = now;
  return WIFIRETRY_BEGIN;
}

bool wifiretry_ap_may_start(const struct wifiretry_t *r, unsigned long now) {
  return (unsigned long)(now - r->last) >= WIFIRETRY_AP_MS;
}
