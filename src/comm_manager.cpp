/*
 * Copyright (c) 2017, James Jackson and Daniel Koch, BYU MAGICC Lab
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <string.h>

#include "rosflight.h"

namespace rosflight_firmware
{

CommManager::CommManager(ROSflight& rf, CommLink& comm_link) :
  RF_(rf),
  comm_link_(comm_link)
{}

// function definitions
void CommManager::init()
{
  comm_link_.register_param_request_list_callback([this](uint8_t target_system){this->param_request_list_callback(target_system);});
  comm_link_.register_param_request_read_callback([this](uint8_t target_system, const char* const param_name, int16_t param_index){this->param_request_read_callback(target_system, param_name, param_index);});
  comm_link_.register_param_set_int_callback([this](uint8_t target_system, const char* const param_name, int32_t param_value){this->param_set_int_callback(target_system, param_name, param_value);});
  comm_link_.register_param_set_float_callback([this](uint8_t target_system, const char* const param_name, float param_value){this->param_set_float_callback(target_system, param_name, param_value);});
  comm_link_.register_offboard_control_callback([this](const CommLink::OffboardControl& control){this->offboard_control_callback(control);});
  comm_link_.register_command_callback([this](CommLink::Command command){this->command_callback(command);});
  comm_link_.register_timesync_callback([this](int64_t tc1, int64_t ts1){this->timesync_callback(tc1, ts1);});
  comm_link_.register_attitude_correction_callback([this](const turbomath::Quaternion& q){this->attitude_correction_callback(q);});
  comm_link_.register_heartbeat_callback([this](void){this->heartbeat_callback();});
  comm_link_.init(static_cast<uint32_t>(RF_.params_.get_param_int(PARAM_BAUD_RATE)),
                  static_cast<uint32_t>(RF_.params_.get_param_int(PARAM_SERIAL_DEVICE)));

  sysid_ = static_cast<uint8_t>(RF_.params_.get_param_int(PARAM_SYSTEM_ID));

  offboard_control_time_ = 0;
  send_params_index_ = PARAMS_COUNT;

  // Register Param change callbacks
  RF_.params_.add_callback([this](int16_t param_id){this->update_system_id(param_id);}, PARAM_SYSTEM_ID);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_HEARTBEAT, param_id);}, PARAM_STREAM_HEARTBEAT_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_STATUS, param_id);}, PARAM_STREAM_STATUS_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_IMU, param_id);}, PARAM_STREAM_IMU_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_ATTITUDE, param_id);}, PARAM_STREAM_ATTITUDE_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_DIFF_PRESSURE, param_id);}, PARAM_STREAM_AIRSPEED_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_BARO, param_id);}, PARAM_STREAM_BARO_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_SONAR, param_id);}, PARAM_STREAM_SONAR_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_MAG, param_id);}, PARAM_STREAM_MAG_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_SERVO_OUTPUT_RAW, param_id);}, PARAM_STREAM_OUTPUT_RAW_RATE);
  RF_.params_.add_callback([this](int16_t param_id){this->set_streaming_rate(STREAM_ID_RC_RAW, param_id);}, PARAM_STREAM_RC_RAW_RATE);

  initialized_ = true;
  log(CommLink::LogSeverity::LOG_INFO, "Booting");
}

void CommManager::update_system_id(uint16_t param_id)
{
  (void) param_id;
  sysid_ = static_cast<uint8_t>(RF_.params_.get_param_int(PARAM_SYSTEM_ID));
}

void CommManager::update_status()
{
  send_status();
}

void CommManager::send_param_value(uint16_t param_id)
{
  if (param_id < PARAMS_COUNT)
  {
    switch (RF_.params_.get_param_type(param_id))
    {
    case PARAM_TYPE_INT32:
      comm_link_.send_param_value_int(sysid_,
                                      param_id,
                                      RF_.params_.get_param_name(param_id),
                                      RF_.params_.get_param_int(param_id),
                                      static_cast<uint16_t>(PARAMS_COUNT));
      break;
    case PARAM_TYPE_FLOAT:
      comm_link_.send_param_value_float(sysid_,
                                        param_id,
                                        RF_.params_.get_param_name(param_id),
                                        RF_.params_.get_param_float(param_id),
                                        static_cast<uint16_t>(PARAMS_COUNT));
      break;
    default:
      break;
    }
  }
}

void CommManager::param_request_list_callback(uint8_t target_system)
{
  if (target_system == sysid_)
    send_params_index_ = 0;
}

void CommManager::send_parameter_list()
{
  send_params_index_ = 0;
}

void CommManager::param_request_read_callback(uint8_t target_system, const char* const param_name, int16_t param_index)
{
  if (target_system == sysid_)
  {
    uint16_t id = (param_index < 0) ? RF_.params_.lookup_param_id(param_name) : static_cast<uint16_t>(param_index);

    if (id < PARAMS_COUNT)
      send_param_value(id);
  }
}

void CommManager::param_set_int_callback(uint8_t target_system, const char* const param_name, int32_t param_value)
{
  if (target_system == sysid_)
  {
    uint16_t id = RF_.params_.lookup_param_id(param_name);

    if (id < PARAMS_COUNT && RF_.params_.get_param_type(id) == PARAM_TYPE_INT32)
    {
      RF_.params_.set_param_int(id, param_value);
    }
  }
}

void CommManager::param_set_float_callback(uint8_t target_system, const char* const param_name, float param_value)
{
  if (target_system == sysid_)
  {
    uint16_t id = RF_.params_.lookup_param_id(param_name);

    if (id < PARAMS_COUNT && RF_.params_.get_param_type(id) == PARAM_TYPE_FLOAT)
    {
      RF_.params_.set_param_float(id, param_value);
    }
  }
}

void CommManager::command_callback(CommLink::Command command)
{
  bool result;
  bool reboot_flag = false;
  bool reboot_to_bootloader_flag = false;

  // None of these actions can be performed if we are armed
  if (RF_.state_manager_.state().armed)
  {
    result = false;
  }
  else
  {
    result = true;
    switch (command)
    {
    case CommLink::Command::COMMAND_READ_PARAMS:
      result = RF_.params_.read();
      break;
    case CommLink::Command::COMMAND_WRITE_PARAMS:
      result = RF_.params_.write();
      break;
    case CommLink::Command::COMMAND_SET_PARAM_DEFAULTS:
      RF_.params_.set_defaults();
      break;
    case CommLink::Command::COMMAND_ACCEL_CALIBRATION:
      result = RF_.sensors_.start_imu_calibration();
      break;
    case CommLink::Command::COMMAND_GYRO_CALIBRATION:
      result = RF_.sensors_.start_gyro_calibration();
      break;
    case CommLink::Command::COMMAND_BARO_CALIBRATION:
      result = RF_.sensors_.start_baro_calibration();
      break;
    case CommLink::Command::COMMAND_AIRSPEED_CALIBRATION:
      result = RF_.sensors_.start_diff_pressure_calibration();
      break;
    case CommLink::Command::COMMAND_RC_CALIBRATION:
      RF_.controller_.calculate_equilbrium_torque_from_rc();
      break;
    case CommLink::Command::COMMAND_REBOOT:
      reboot_flag = true;
      break;
    case CommLink::Command::COMMAND_REBOOT_TO_BOOTLOADER:
      reboot_to_bootloader_flag = true;
      break;
    case CommLink::Command::COMMAND_SEND_VERSION:
      comm_link_.send_version(sysid_, GIT_VERSION_STRING);
      break;
    }
  }

  comm_link_.send_command_ack(sysid_, command, result);

  if (reboot_flag || reboot_to_bootloader_flag)
  {
    RF_.board_.clock_delay(20);
    RF_.board_.board_reset(reboot_to_bootloader_flag);
  }
  RF_.board_.serial_flush();
}

void CommManager::timesync_callback(int64_t tc1, int64_t ts1)
{
  uint64_t now_us = RF_.board_.clock_micros();

  if (tc1 == 0) // check that this is a request, not a response
    comm_link_.send_timesync(sysid_, static_cast<int64_t>(now_us)*1000, ts1);
}

void CommManager::offboard_control_callback(const CommLink::OffboardControl& control)
{
  // put values into a new command struct
  control_t new_offboard_command;
  new_offboard_command.x.value = control.x.value;
  new_offboard_command.y.value = control.y.value;
  new_offboard_command.z.value = control.z.value;
  new_offboard_command.F.value = control.F.value;

  // Move flags into standard message
  new_offboard_command.x.active = control.x.valid;
  new_offboard_command.y.active = control.y.valid;
  new_offboard_command.z.active = control.z.valid;
  new_offboard_command.F.active = control.F.valid;

  // translate modes into standard message
  switch (control.mode)
  {
  case CommLink::OffboardControl::Mode::PASS_THROUGH:
    new_offboard_command.x.type = PASSTHROUGH;
    new_offboard_command.y.type = PASSTHROUGH;
    new_offboard_command.z.type = PASSTHROUGH;
    new_offboard_command.F.type = THROTTLE;
    break;
  case CommLink::OffboardControl::Mode::ROLLRATE_PITCHRATE_YAWRATE_THROTTLE:
    new_offboard_command.x.type = RATE;
    new_offboard_command.y.type = RATE;
    new_offboard_command.z.type = RATE;
    new_offboard_command.F.type = THROTTLE;
    break;
  case CommLink::OffboardControl::Mode::ROLL_PITCH_YAWRATE_THROTTLE:
    new_offboard_command.x.type = ANGLE;
    new_offboard_command.y.type = ANGLE;
    new_offboard_command.z.type = RATE;
    new_offboard_command.F.type = THROTTLE;
    break;
  }

  // Tell the command_manager that we have a new command we need to mux
  new_offboard_command.stamp_ms = RF_.board_.clock_millis();
  RF_.command_manager_.set_new_offboard_command(new_offboard_command);
}

void CommManager::attitude_correction_callback(const turbomath::Quaternion &q)
{
  RF_.estimator_.set_attitude_correction(q);
}

void CommManager::heartbeat_callback(void)
{
  // receiving a heartbeat implies that a connection has been made
  // to the off-board computer.
  connected_ = true;

  static bool one_time_data_sent = false;
  if (!one_time_data_sent)
  {
    // error data
    if (this->RF_.board_.has_backup_data())
    {
        this->send_error_data();
    }

    // buffered log messages
    uint8_t log_size = log_buffer_size();
    while (log_size--)
    {
      log(log_severity_buffer_[log_buffer_tail_], log_buffer_[log_buffer_tail_]);
      log_buffer_tail_ = (log_buffer_tail_ + 1) % LOG_BUF_SIZE;
    }
    log_buffer_head_ = log_buffer_tail_ = 0;
    log_buffer_full_ = false;

    one_time_data_sent = true;
  }

  // respond to heartbeats with a heartbeat
  this->send_heartbeat();
}

// function definitions
void CommManager::receive(void)
{
  comm_link_.receive();
}

void CommManager::log(CommLink::LogSeverity severity, const char *fmt, ...)
{
  // Convert the format string to a raw char array
  va_list args;
  va_start(args, fmt);
  char text[LOG_MSG_SIZE];
  rosflight_firmware::nanoprintf::tfp_sprintf(text, fmt, args);
  va_end(args);

  if (initialized_ && connected_)
  {
    comm_link_.send_log_message(sysid_, severity, text);
  }
  else
  {
    // Add log message to circular buffer
    memcpy(log_buffer_[log_buffer_head_], text, LOG_MSG_SIZE);
    log_severity_buffer_[log_buffer_head_] = severity;
    // advance indices, keeping read/write indices ordered
    if (log_buffer_full_) log_buffer_tail_ = (log_buffer_tail_ + 1) % LOG_BUF_SIZE;
    log_buffer_head_ = (log_buffer_head_ + 1) % LOG_BUF_SIZE;
    log_buffer_full_ = (log_buffer_head_ == log_buffer_tail_);
  }
}

void CommManager::send_heartbeat(void)
{
  comm_link_.send_heartbeat(sysid_, static_cast<bool>(RF_.params_.get_param_int(PARAM_FIXED_WING)));
}

void CommManager::send_status(void)
{
  if (!initialized_)
    return;

  uint8_t control_mode = 0;
  if (RF_.params_.get_param_int(PARAM_FIXED_WING))
    control_mode = MODE_PASS_THROUGH;
  else if (RF_.command_manager_.combined_control().x.type == ANGLE)
    control_mode = MODE_ROLL_PITCH_YAWRATE_THROTTLE;
  else
    control_mode = MODE_ROLLRATE_PITCHRATE_YAWRATE_THROTTLE;

  comm_link_.send_status(sysid_,
                         RF_.state_manager_.state().armed,
                         RF_.state_manager_.state().failsafe,
                         RF_.command_manager_.rc_override_active(),
                         RF_.command_manager_.offboard_control_active(),
                         RF_.state_manager_.state().error_codes,
                         control_mode,
                         RF_.board_.num_sensor_errors(),
                         RF_.get_loop_time_us());
}


void CommManager::send_attitude(void)
{
  comm_link_.send_attitude_quaternion(sysid_,
                                      RF_.estimator_.state().timestamp_us,
                                      RF_.estimator_.state().attitude,
                                      RF_.estimator_.state().angular_velocity);
}

void CommManager::send_imu(void)
{
  turbomath::Vector acc, gyro;
  uint64_t stamp_us;
  RF_.sensors_.get_filtered_IMU(acc, gyro, stamp_us);
  comm_link_.send_imu(sysid_,
                      stamp_us,
                      acc,
                      gyro,
                      RF_.sensors_.data().imu_temperature);

}

void CommManager::send_output_raw(void)
{
  comm_link_.send_output_raw(sysid_,
                             RF_.board_.clock_millis(),
                             RF_.mixer_.get_outputs());
}

void CommManager::send_rc_raw(void)
{
  // TODO better mechanism for retreiving RC (through RC module, not PWM-specific)
  uint16_t channels[8] = { static_cast<uint16_t>(RF_.board_.rc_read(0)*1000),
                           static_cast<uint16_t>(RF_.board_.rc_read(1)*1000),
                           static_cast<uint16_t>(RF_.board_.rc_read(2)*1000),
                           static_cast<uint16_t>(RF_.board_.rc_read(3)*1000),
                           static_cast<uint16_t>(RF_.board_.rc_read(4)*1000),
                           static_cast<uint16_t>(RF_.board_.rc_read(5)*1000),
                           static_cast<uint16_t>(RF_.board_.rc_read(6)*1000),
                           static_cast<uint16_t>(RF_.board_.rc_read(7)*1000) };
  comm_link_.send_rc_raw(sysid_, RF_.board_.clock_millis(), channels);
}

void CommManager::send_diff_pressure(void)
{
  if (RF_.sensors_.data().diff_pressure_valid)
  {
    comm_link_.send_diff_pressure(sysid_,
                                  RF_.sensors_.data().diff_pressure_velocity,
                                  RF_.sensors_.data().diff_pressure,
                                  RF_.sensors_.data().diff_pressure_temp);
  }
}

void CommManager::send_baro(void)
{
  if (RF_.sensors_.data().baro_valid)
  {
    comm_link_.send_baro(sysid_,
                         RF_.sensors_.data().baro_altitude,
                         RF_.sensors_.data().baro_pressure,
                         RF_.sensors_.data().baro_temperature);
  }
}

void CommManager::send_sonar(void)
{
  if (RF_.sensors_.data().sonar_range_valid)
  {
    comm_link_.send_sonar(sysid_,
                          0, // TODO set sensor type (sonar/lidar), use enum
                          RF_.sensors_.data().sonar_range,
                          8.0,
                          0.25);
  }
}

void CommManager::send_mag(void)
{
  if (RF_.sensors_.data().mag_present)
    comm_link_.send_mag(sysid_, RF_.sensors_.data().mag);
}
void CommManager::send_error_data(void)
{
  BackupData error_data = RF_.board_.get_backup_data();
  comm_link_.send_error_data(sysid_, error_data);
}

void CommManager::send_low_priority(void)
{
  send_next_param();
}

// function definitions
void CommManager::stream()
{
  uint64_t time_us = RF_.board_.clock_micros();
  for (int i = 0; i < STREAM_COUNT; i++)
  {
    streams_[i].stream(time_us);
  }
  RF_.board_.serial_flush();
}

void CommManager::set_streaming_rate(uint8_t stream_id, int16_t param_id)
{
  streams_[stream_id].set_rate(RF_.params_.get_param_int(param_id));
}

void CommManager::send_named_value_int(const char *const name, int32_t value)
{
  comm_link_.send_named_value_int(sysid_, RF_.board_.clock_millis(), name, value);
}

void CommManager::send_named_value_float(const char *const name, float value)
{
  comm_link_.send_named_value_float(sysid_, RF_.board_.clock_millis(), name, value);
}

void CommManager::send_next_param(void)
{
  if (send_params_index_ < PARAMS_COUNT)
  {
    send_param_value(static_cast<uint16_t>(send_params_index_));
    send_params_index_++;
  }
}

uint8_t CommManager::log_buffer_size() const
{
  uint8_t size = LOG_BUF_SIZE;
  if (!log_buffer_full_)
  {
    if (log_buffer_head_ >= log_buffer_tail_)
    {
      size = log_buffer_head_ - log_buffer_tail_;
    }
    else
    {
      size = LOG_BUF_SIZE + log_buffer_head_ - log_buffer_tail_;
    }
  }
  return size;
}

CommManager::Stream::Stream(uint32_t period_us, std::function<void(void)> send_function) :
  period_us_(period_us),
  next_time_us_(0),
  send_function_(send_function)
{}

void CommManager::Stream::stream(uint64_t now_us)
{
  if (period_us_ > 0 && now_us >= next_time_us_)
  {
    // if you fall behind, skip messages
    do
    {
      next_time_us_ += period_us_;
    }
    while(next_time_us_ < now_us);

    send_function_();
  }
}

void CommManager::Stream::set_rate(uint32_t rate_hz)
{
  period_us_ = (rate_hz == 0) ? 0 : 1000000/rate_hz;
}

//void Mavlink::mavlink_send_named_command_struct(const char *const name, control_t command_struct)
//{
//  uint8_t control_mode;
//  if (command_struct.x.type == RATE && command_struct.y.type == RATE)
//  {
//    control_mode = MODE_ROLLRATE_PITCHRATE_YAWRATE_THROTTLE;
//  }
//  else if (command_struct.x.type == ANGLE && command_struct.y.type == ANGLE)
//  {
//    if (command_struct.x.type == ALTITUDE)
//    {
//      control_mode = MODE_ROLL_PITCH_YAWRATE_ALTITUDE;
//    }
//    else
//    {
//      control_mode = MODE_ROLL_PITCH_YAWRATE_THROTTLE;
//    }
//  }
//  else
//  {
//    control_mode = MODE_PASS_THROUGH;
//  }
//  uint8_t ignore = !(command_struct.x.active) ||
//                   !(command_struct.y.active) << 1 ||
//                   !(command_struct.z.active) << 2 ||
//                   !(command_struct.F.active) << 3;
//  mavlink_message_t msg;
//  mavlink_msg_named_command_struct_pack(sysid, compid, &msg, name,
//                                        control_mode,
//                                        ignore,
//                                        command_struct.x.value,
//                                        command_struct.y.value,
//                                        command_struct.z.value,
//                                        command_struct.F.value);
//  send_message(msg);
//}

} // namespace rosflight_firmware
