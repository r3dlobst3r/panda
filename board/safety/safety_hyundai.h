const int HYUNDAI_MAX_STEER = 255;             // like stock
const int HYUNDAI_MAX_RT_DELTA = 112;          // max delta torque allowed for real time checks
const uint32_t HYUNDAI_RT_INTERVAL = 250000;   // 250ms between real time checks
const int HYUNDAI_MAX_RATE_UP = 3;
const int HYUNDAI_MAX_RATE_DOWN = 7;
const int HYUNDAI_DRIVER_TORQUE_ALLOWANCE = 50;
const int HYUNDAI_DRIVER_TORQUE_FACTOR = 2;
const int HYUNDAI_STANDSTILL_THRSLD = 30;  // ~1kph
const CanMsg HYUNDAI_TX_MSGS[] = {{832, 0, 8}, {1265, 0, 4}, {1157, 0, 4}};

// TODO: do checksum checks
AddrCheckStruct hyundai_rx_checks[] = {
  {.msg = {{608, 0, 8, .check_checksum = true, .max_counter = 3U, .expected_timestep = 10000U},
           {881, 0, 8, .expected_timestep = 10000U}}},
  {.msg = {{897, 0, 8, .max_counter = 255U, .expected_timestep = 10000U}}},
  {.msg = {{902, 0, 8, .max_counter = 15U,  .expected_timestep = 10000U}}},
  {.msg = {{916, 0, 8, .check_checksum = false, .max_counter = 7U, .expected_timestep = 10000U}}},
  {.msg = {{1057, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}}},
};
const int HYUNDAI_RX_CHECK_LEN = sizeof(hyundai_rx_checks) / sizeof(hyundai_rx_checks[0]);

static uint8_t hyundai_get_counter(CAN_FIFOMailBox_TypeDef *to_push) {
  int addr = GET_ADDR(to_push);

  uint8_t cnt;
  if (addr == 608) {
    cnt = (GET_BYTE(to_push, 7) >> 4) & 0x3;
  } else if (addr == 897) {
    cnt = GET_BYTE(to_push, 5);
  } else if (addr == 902) {
    cnt = ((GET_BYTE(to_push, 3) >> 6) << 2) | (GET_BYTE(to_push, 1) >> 6);
  } else if (addr == 916) {
    cnt = (GET_BYTE(to_push, 1) >> 5) & 0x7;
  } else if (addr == 1057) {
    cnt = GET_BYTE(to_push, 7) & 0xF;
  } else {
    cnt = 0;
  }
  return cnt;
}

static uint8_t hyundai_get_checksum(CAN_FIFOMailBox_TypeDef *to_push) {
  int addr = GET_ADDR(to_push);

  uint8_t chksum;
  if (addr == 608) {
    chksum = GET_BYTE(to_push, 7) & 0xF;
  } else if (addr == 916) {
    chksum = GET_BYTE(to_push, 6) & 0xF;
  } else if (addr == 1057) {
    chksum = GET_BYTE(to_push, 7) >> 4;
  } else {
    chksum = 0;
  }
  return chksum;
}

static uint8_t hyundai_compute_checksum(CAN_FIFOMailBox_TypeDef *to_push) {
  int addr = GET_ADDR(to_push);

  uint8_t chksum = 0;
  // same algorithm, but checksum is in a different place
  for (int i = 0; i < 8; i++) {
    uint8_t b = GET_BYTE(to_push, i);
    if (((addr == 608) && (i == 7)) || ((addr == 916) && (i == 6)) || ((addr == 1057) && (i == 7))) {
      b &= (addr == 1057) ? 0x0FU : 0xF0U; // remove checksum
    }
    chksum += (b % 16U) + (b / 16U);
  }
  chksum = (16U - (chksum %  16U)) % 16U;
  return chksum;
}

static int hyundai_rx_hook(CAN_FIFOMailBox_TypeDef *to_push) {

  bool valid = addr_safety_check(to_push, hyundai_rx_checks, HYUNDAI_RX_CHECK_LEN,
                                 hyundai_get_checksum, hyundai_compute_checksum,
                                 hyundai_get_counter);

  bool unsafe_allow_gas = unsafe_mode & UNSAFE_DISABLE_DISENGAGE_ON_GAS;

  int addr = GET_ADDR(to_push);
  int bus = GET_BUS(to_push);

  if (valid && (bus == 0)) {
    if (addr == 593) {
      int torque_driver_new = ((GET_BYTES_04(to_push) & 0x7ff) * 0.79) - 808; // scale down new driver torque signal to match previous one
      // update array of samples
      update_sample(&torque_driver, torque_driver_new);
    }

    // enter controls on rising edge of ACC, exit controls on ACC off
    if (addr == 1057) {
      // 2 bits: 13-14
      int cruise_engaged = (GET_BYTES_04(to_push) >> 13) & 0x3;
      if (cruise_engaged && !cruise_engaged_prev) {
        controls_allowed = 1;
      }
      if (!cruise_engaged) {
        controls_allowed = 0;
      }
      cruise_engaged_prev = cruise_engaged;
    }

    // exit controls on rising edge of gas press
    if ((addr == 608) || (addr == 881)) {
      bool gas_pressed;
      if (addr == 608) {
        gas_pressed = (GET_BYTE(to_push, 7) >> 6) != 0;
      } else {
        gas_pressed = (((GET_BYTE(to_push, 4) & 0x7F) << 1) | GET_BYTE(to_push, 3) >> 6) != 0;
      }

      if (!unsafe_allow_gas && gas_pressed && !gas_pressed_prev) {
        controls_allowed = 0;
      }
      gas_pressed_prev = gas_pressed;
    }

    // sample subaru wheel speed, averaging opposite corners
    if (addr == 902) {
      int hyundai_speed = GET_BYTES_04(to_push) & 0x3FFF;  // FL
      hyundai_speed += (GET_BYTES_48(to_push) >> 16) & 0x3FFF;  // RL
      hyundai_speed /= 2;
      vehicle_moving = hyundai_speed > HYUNDAI_STANDSTILL_THRSLD;
    }

    // exit controls on rising edge of brake press
    if (addr == 916) {
      bool brake_pressed = (GET_BYTE(to_push, 6) >> 7) != 0;
      if (brake_pressed && (!brake_pressed_prev || vehicle_moving)) {
        controls_allowed = 0;
      }
      brake_pressed_prev = brake_pressed;
    }

    // check if stock camera ECU is on bus 0
    if ((safety_mode_cnt > RELAY_TRNS_TIMEOUT) && (addr == 832)) {
      relay_malfunction_set();
    }
  }
  return valid;
}

static int hyundai_tx_hook(CAN_FIFOMailBox_TypeDef *to_send) {

  int tx = 1;
  int addr = GET_ADDR(to_send);

  if (!msg_allowed(to_send, HYUNDAI_TX_MSGS, sizeof(HYUNDAI_TX_MSGS)/sizeof(HYUNDAI_TX_MSGS[0]))) {
    tx = 0;
  }

  if (relay_malfunction) {
    tx = 0;
  }

  // LKA STEER: safety check
  if (addr == 832) {
    int desired_torque = ((GET_BYTES_04(to_send) >> 16) & 0x7ff) - 1024;
    uint32_t ts = TIM2->CNT;
    bool violation = 0;

    if (controls_allowed) {

      // *** global torque limit check ***
      violation |= max_limit_check(desired_torque, HYUNDAI_MAX_STEER, -HYUNDAI_MAX_STEER);

      // *** torque rate limit check ***
      violation |= driver_limit_check(desired_torque, desired_torque_last, &torque_driver,
        HYUNDAI_MAX_STEER, HYUNDAI_MAX_RATE_UP, HYUNDAI_MAX_RATE_DOWN,
        HYUNDAI_DRIVER_TORQUE_ALLOWANCE, HYUNDAI_DRIVER_TORQUE_FACTOR);

      // used next time
      desired_torque_last = desired_torque;

      // *** torque real time rate limit check ***
      violation |= rt_rate_limit_check(desired_torque, rt_torque_last, HYUNDAI_MAX_RT_DELTA);

      // every RT_INTERVAL set the new limits
      uint32_t ts_elapsed = get_ts_elapsed(ts, ts_last);
      if (ts_elapsed > HYUNDAI_RT_INTERVAL) {
        rt_torque_last = desired_torque;
        ts_last = ts;
      }
    }

    // no torque if controls is not allowed
    if (!controls_allowed && (desired_torque != 0)) {
      violation = 1;
    }

    // reset to 0 if either controls is not allowed or there's a violation
    if (violation || !controls_allowed) {
      desired_torque_last = 0;
      rt_torque_last = 0;
      ts_last = ts;
    }

    if (violation) {
      tx = 0;
    }
  }

  // FORCE CANCEL: safety check only relevant when spamming the cancel button.
  // ensuring that only the cancel button press is sent (VAL 4) when controls are off.
  // This avoids unintended engagements while still allowing resume spam
  if ((addr == 1265) && !controls_allowed) {
    if ((GET_BYTES_04(to_send) & 0x7) != 4) {
      tx = 0;
    }
  }

  // 1 allows the message through
  return tx;
}

static int hyundai_fwd_hook(int bus_num, CAN_FIFOMailBox_TypeDef *to_fwd) {

  int bus_fwd = -1;
  int addr = GET_ADDR(to_fwd);
  // forward cam to ccan and viceversa, except lkas cmd
  if (!relay_malfunction) {
    if (bus_num == 0) {
      bus_fwd = 2;
    }
    if ((bus_num == 2) && (addr != 832) && (addr != 1157)) {
      bus_fwd = 0;
    }
  }
  return bus_fwd;
}

const safety_hooks hyundai_hooks = {
  .init = nooutput_init,
  .rx = hyundai_rx_hook,
  .tx = hyundai_tx_hook,
  .tx_lin = nooutput_tx_lin_hook,
  .fwd = hyundai_fwd_hook,
  .addr_check = hyundai_rx_checks,
  .addr_check_len = sizeof(hyundai_rx_checks) / sizeof(hyundai_rx_checks[0]),
};
