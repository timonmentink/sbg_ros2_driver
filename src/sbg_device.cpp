// File header
#include "sbg_device.h"

// Standard headers
#include <iomanip>
#include <fstream>
#include <ctime>

// SbgECom headers
#include <version/sbgVersion.h>

using namespace std;
using sbg::SbgDevice;

// From ros_com/recorder
std::string timeToStr()
{
    std::stringstream msg;
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto tm = *std::localtime(&now);
    msg << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S");
    return msg.str();
}

//
// Static magnetometers maps definition.
//
std::map<SbgEComMagCalibQuality, std::string> SbgDevice::g_mag_calib_quality_ = { {SBG_ECOM_MAG_CALIB_QUAL_OPTIMAL, "Quality: optimal"},
                                                                                  {SBG_ECOM_MAG_CALIB_QUAL_GOOD, "Quality: good"},
                                                                                  {SBG_ECOM_MAG_CALIB_QUAL_POOR, "Quality: poor"},
                                                                                  {SBG_ECOM_MAG_CALIB_QUAL_INVALID, "Quality: invalid"}};

std::map<SbgEComMagCalibConfidence, std::string> SbgDevice::g_mag_calib_confidence_ = { {SBG_ECOM_MAG_CALIB_TRUST_HIGH, "Confidence: high"},
                                                                                        {SBG_ECOM_MAG_CALIB_TRUST_MEDIUM, "Confidence: medium"},
                                                                                        {SBG_ECOM_MAG_CALIB_TRUST_LOW, "Confidence: low"}};

std::map<SbgEComMagCalibMode, std::string> SbgDevice::g_mag_calib_mode_ = { {SBG_ECOM_MAG_CALIB_MODE_2D, "Mode 2D"},
                                                                            {SBG_ECOM_MAG_CALIB_MODE_3D, "Mode 3D"}};

std::map<SbgEComMagCalibBandwidth, std::string> SbgDevice::g_mag_calib_bandwidth = {{SBG_ECOM_MAG_CALIB_HIGH_BW, "High Bandwidth"},
                                                                                    {SBG_ECOM_MAG_CALIB_MEDIUM_BW, "Medium Bandwidth"},
                                                                                    {SBG_ECOM_MAG_CALIB_LOW_BW, "Low Bandwidth"}};

/*!
 * Class to handle a connected SBG device.
 */
//---------------------------------------------------------------------//
//- Constructor                                                       -//
//---------------------------------------------------------------------//

SbgDevice::SbgDevice(rclcpp::Node& ref_node_handle):
m_ref_node_(ref_node_handle),
m_mag_calibration_ongoing_(false),
m_mag_calibration_done_(false)
{
  loadParameters();
  connect();
}

SbgDevice::~SbgDevice(void)
{
  SbgErrorCode error_code;

  error_code = sbgEComClose(&m_com_handle_);

  if (error_code != SBG_NO_ERROR)
  {
    RCLCPP_ERROR(m_ref_node_.get_logger(), "Unable to close the SBG communication handle - %s.", sbgErrorCodeToString(error_code));
  }

  if (m_config_store_.isInterfaceSerial())
  {
    error_code = sbgInterfaceSerialDestroy(&m_sbg_interface_);
  }
  else if (m_config_store_.isInterfaceUdp())
  {
    error_code = sbgInterfaceUdpDestroy(&m_sbg_interface_);
  }

  if (error_code != SBG_NO_ERROR)
  {
    RCLCPP_ERROR(m_ref_node_.get_logger(), "SBG DRIVER - Unable to close the communication interface.");
  }
}

//---------------------------------------------------------------------//
//- Private  methods                                                  -//
//---------------------------------------------------------------------//

SbgErrorCode SbgDevice::onLogReceivedCallback(SbgEComHandle* p_handle, SbgEComClass msg_class, SbgEComMsgId msg, const SbgBinaryLogData* p_log_data, void* p_user_arg)
{
  assert(p_user_arg);

  SBG_UNUSED_PARAMETER(p_handle);

  SbgDevice *p_sbg_device;
  p_sbg_device = (SbgDevice*)(p_user_arg);

  p_sbg_device->onLogReceived(msg_class, msg, *p_log_data);

  return SBG_NO_ERROR;
}

void SbgDevice::onLogReceived(SbgEComClass msg_class, SbgEComMsgId msg, const SbgBinaryLogData& ref_sbg_data)
{
  //
  // Publish the received SBG log.
  //
  m_message_publisher_.publish(msg_class, msg, ref_sbg_data);
}

void SbgDevice::loadParameters(void)
{
  //
  // Get the ROS private nodeHandle, where the parameters are loaded from the launch file.
  //
  rclcpp::NodeOptions node_opt;
  node_opt.automatically_declare_parameters_from_overrides(true);
  rclcpp::Node n_private("npv", "", node_opt);
  m_config_store_.loadFromRosNodeHandle(n_private);
}

void SbgDevice::connect(void)
{
  SbgErrorCode error_code;
  error_code = SBG_NO_ERROR;

  //
  // Initialize the communication interface from the config store, then initialize the sbgECom protocol to communicate with the device.
  //
  if (m_config_store_.isInterfaceSerial())
  {
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG_DRIVER - serial interface %s at %d bps", m_config_store_.getUartPortName().c_str(), m_config_store_.getBaudRate());
    error_code = sbgInterfaceSerialCreate(&m_sbg_interface_, m_config_store_.getUartPortName().c_str(), m_config_store_.getBaudRate());
  }
  else if (m_config_store_.isInterfaceUdp())
  {
	char ip[16];
	sbgNetworkIpToString(m_config_store_.getIpAddress(), ip, sizeof(ip));
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG_DRIVER - UDP interface %s %d->%d", ip, m_config_store_.getInputPortAddress(), m_config_store_.getOutputPortAddress());
    error_code = sbgInterfaceUdpCreate(&m_sbg_interface_, m_config_store_.getIpAddress(), m_config_store_.getInputPortAddress(), m_config_store_.getOutputPortAddress());
  }
  else
  {
    rclcpp::exceptions::throw_from_rcl_error(RCL_RET_ERROR, "Invalid interface type for the SBG device.");
  }

  if (error_code != SBG_NO_ERROR)
  {
    rclcpp::exceptions::throw_from_rcl_error(RCL_RET_ERROR, "SBG_DRIVER - [Init] Unable to initialize the interface - " + std::string(sbgErrorCodeToString(error_code)));
  }

  error_code = sbgEComInit(&m_com_handle_, &m_sbg_interface_);

  if (error_code != SBG_NO_ERROR)
  {
    rclcpp::exceptions::throw_from_rcl_error(RCL_RET_ERROR, "SBG_DRIVER - [Init] Unable to initialize the SbgECom protocol - " + std::string(sbgErrorCodeToString(error_code)));
  }

  readDeviceInfo();
}

void SbgDevice::readDeviceInfo(void)
{
  SbgEComDeviceInfo device_info;
  SbgErrorCode      error_code;

  error_code = sbgEComCmdGetInfo(&m_com_handle_, &device_info);

  if (error_code == SBG_NO_ERROR)
  {
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG_DRIVER - productCode = %s", device_info.productCode);
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG_DRIVER - serialNumber = %u", device_info.serialNumber);

    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG_DRIVER - calibationRev = %s", getVersionAsString(device_info.calibationRev).c_str());
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG_DRIVER - calibrationDate = %u / %u / %u", device_info.calibrationDay, device_info.calibrationMonth, device_info.calibrationYear);

    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG_DRIVER - hardwareRev = %s", getVersionAsString(device_info.hardwareRev).c_str());
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG_DRIVER - firmwareRev = %s", getVersionAsString(device_info.firmwareRev).c_str());
  }
  else
  {
    RCLCPP_ERROR(m_ref_node_.get_logger(), "Unable to get the device Info : %s", sbgErrorCodeToString(error_code));
  }
}

std::string SbgDevice::getVersionAsString(uint32 sbg_version_enc) const
{
  char version[32];
  sbgVersionToStringEncoded(sbg_version_enc, version, 32);

  return std::string(version);
}

void SbgDevice::initPublishers(void)
{
  m_message_publisher_.initPublishers(m_ref_node_, m_config_store_);

  m_rate_frequency_ = m_config_store_.getReadingRateFrequency();
}

void SbgDevice::configure(void)
{
  if (m_config_store_.checkConfigWithRos())
  {
    ConfigApplier configApplier(m_com_handle_);
    configApplier.applyConfiguration(m_config_store_);
  }
}

bool SbgDevice::processMagCalibration(const std::shared_ptr<std_srvs::srv::Trigger::Request> ref_ros_request, std::shared_ptr<std_srvs::srv::Trigger::Response> ref_ros_response)
{
  SBG_UNUSED_PARAMETER(ref_ros_request);

  if (m_mag_calibration_ongoing_)
  {
    if (endMagCalibration())
    {
      ref_ros_response->success = true;
      ref_ros_response->message = "Magnetometer calibration is finished. See the output console to get calibration informations.";
    }
    else
    {
      ref_ros_response->success = false;
      ref_ros_response->message = "Unable to end the calibration.";
    }

    m_mag_calibration_ongoing_  = false;
    m_mag_calibration_done_     = true;
  }
  else
  {
    if (startMagCalibration())
    {
      ref_ros_response->success = true;
      ref_ros_response->message = "Magnetometer calibration process started.";
    }
    else
    {
      ref_ros_response->success = false;
      ref_ros_response->message = "Unable to start magnetometers calibration.";
    }

    m_mag_calibration_ongoing_ = true;
  }

  return ref_ros_response->success;
}

bool SbgDevice::saveMagCalibration(const std::shared_ptr<std_srvs::srv::Trigger::Request> ref_ros_request, std::shared_ptr<std_srvs::srv::Trigger::Response> ref_ros_response)
{
  SBG_UNUSED_PARAMETER(ref_ros_request);

  if (m_mag_calibration_ongoing_)
  {
    ref_ros_response->success = false;
    ref_ros_response->message = "Magnetometer calibration process is still ongoing, finish it before trying to save it.";
  }
  else if (m_mag_calibration_done_)
  {
    if (uploadMagCalibrationToDevice())
    {
      ref_ros_response->success = true;
      ref_ros_response->message = "Magnetometer calibration has been uploaded to the device.";
    }
    else
    {
      ref_ros_response->success = false;
      ref_ros_response->message = "Magnetometer calibration has not been uploaded to the device.";
    }
  }
  else
  {
    ref_ros_response->success = false;
    ref_ros_response->message = "No magnetometer calibration has been done.";
  }

  return ref_ros_response->success;
}

bool SbgDevice::startMagCalibration(void)
{
  SbgErrorCode              error_code;
  SbgEComMagCalibMode       mag_calib_mode;
  SbgEComMagCalibBandwidth  mag_calib_bandwidth;

  mag_calib_mode      = m_config_store_.getMagnetometerCalibMode();
  mag_calib_bandwidth = m_config_store_.getMagnetometerCalibBandwidth();

  error_code = sbgEComCmdMagStartCalib(&m_com_handle_, mag_calib_mode, mag_calib_bandwidth);

  if (error_code != SBG_NO_ERROR)
  {
    RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Unable to start the magnetometer calibration : %s", sbgErrorCodeToString(error_code));
    return false;
  }
  else
  {
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Start calibration");
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Mode : %s", g_mag_calib_mode_[mag_calib_mode].c_str());
    RCLCPP_INFO(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Bandwidth : %s", g_mag_calib_bandwidth[mag_calib_bandwidth].c_str());
    return true;
  }
}

bool SbgDevice::endMagCalibration(void)
{
  SbgErrorCode error_code;

  error_code = sbgEComCmdMagComputeCalib(&m_com_handle_, &m_magCalibResults);

  if (error_code != SBG_NO_ERROR)
  {
    RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Unable to compute the magnetometer calibration results : %s", sbgErrorCodeToString(error_code));
    return false;
  }
  else
  {
    displayMagCalibrationStatusResult();
    exportMagCalibrationResults();

    return true;
  }
}

bool SbgDevice::uploadMagCalibrationToDevice(void)
{
  SbgErrorCode error_code;

  if (m_magCalibResults.quality != SBG_ECOM_MAG_CALIB_QUAL_INVALID)
  {
    error_code = sbgEComCmdMagSetCalibData(&m_com_handle_, m_magCalibResults.offset, m_magCalibResults.matrix);

    if (error_code != SBG_NO_ERROR)
    {
      RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Unable to set the magnetometers calibration data to the device : %s", sbgErrorCodeToString(error_code));
      return false;
    }
    else
    {
      RCLCPP_INFO(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Saving data to the device");
      ConfigApplier configApplier(m_com_handle_);
      configApplier.saveConfiguration();
      return true;
    }
  }
  else
  {
    RCLCPP_ERROR(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - The calibration was invalid, it can't be uploaded on the device.");
    return false;
  }
}

void SbgDevice::displayMagCalibrationStatusResult(void) const
{
  RCLCPP_INFO(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Quality of the calibration %s", g_mag_calib_quality_[m_magCalibResults.quality].c_str());
  RCLCPP_INFO(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Calibration results confidence %s", g_mag_calib_confidence_[m_magCalibResults.confidence].c_str());

  SbgEComMagCalibMode mag_calib_mode;

  mag_calib_mode = m_config_store_.getMagnetometerCalibMode();

  //
  // Check the magnetometers calibration status and display the warnings.
  //
  if (m_magCalibResults.advancedStatus & SBG_ECOM_MAG_CALIB_NOT_ENOUGH_POINTS)
  {
    RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Not enough valid points. Maybe you are moving too fast");
  }
  if (m_magCalibResults.advancedStatus & SBG_ECOM_MAG_CALIB_TOO_MUCH_DISTORTIONS)
  {
    RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Unable to find a calibration solution. Maybe there are too much non static distortions");
  }
  if (m_magCalibResults.advancedStatus & SBG_ECOM_MAG_CALIB_ALIGNMENT_ISSUE)
  {
    RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - The magnetic calibration has troubles to correct the magnetometers and inertial frame alignment");
  }
  if (mag_calib_mode == SBG_ECOM_MAG_CALIB_MODE_2D)
  {
    if (m_magCalibResults.advancedStatus & SBG_ECOM_MAG_CALIB_X_MOTION_ISSUE)
    {
      RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Too much roll motion for a 2D magnetic calibration");
    }
    if (m_magCalibResults.advancedStatus & SBG_ECOM_MAG_CALIB_Y_MOTION_ISSUE)
    {
      RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Too much pitch motion for a 2D magnetic calibration");
    }
  }
  else
  {
    if (m_magCalibResults.advancedStatus & SBG_ECOM_MAG_CALIB_X_MOTION_ISSUE)
    {
      RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Not enough roll motion for a 3D magnetic calibration");
    }
    if (m_magCalibResults.advancedStatus & SBG_ECOM_MAG_CALIB_Y_MOTION_ISSUE)
    {
      RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Not enough pitch motion for a 3D magnetic calibration.");
    }
  }
  if (m_magCalibResults.advancedStatus & SBG_ECOM_MAG_CALIB_Z_MOTION_ISSUE)
  {
    RCLCPP_WARN(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Not enough yaw motion to compute a valid magnetic calibration");
  }
}

void SbgDevice::exportMagCalibrationResults(void) const
{
  SbgEComMagCalibMode       mag_calib_mode;
  SbgEComMagCalibBandwidth  mag_calib_bandwidth;
  ostringstream             mag_results_stream;
  string                    output_filename;

  mag_calib_mode      = m_config_store_.getMagnetometerCalibMode();
  mag_calib_bandwidth = m_config_store_.getMagnetometerCalibBandwidth();

  mag_results_stream << "SBG DRIVER [Mag Calib]" << endl;
  mag_results_stream << "======= Parameters =======" << endl;
  mag_results_stream << "* CALIB_MODE = " << g_mag_calib_mode_[mag_calib_mode] << endl;
  mag_results_stream << "* CALIB_BW = " << g_mag_calib_bandwidth[mag_calib_bandwidth] << endl;

  mag_results_stream << "======= Results =======" << endl;
  mag_results_stream << g_mag_calib_quality_[m_magCalibResults.quality] << endl;
  mag_results_stream << g_mag_calib_confidence_[m_magCalibResults.confidence] << endl;
  mag_results_stream << "======= Infos =======" << endl;
  mag_results_stream << "* Used points : " << m_magCalibResults.numPoints << "/" << m_magCalibResults.maxNumPoints << endl;
  mag_results_stream << "* Mean, Std, Max" << endl;
  mag_results_stream << "[Before]\t" << m_magCalibResults.beforeMeanError << "\t" <<  m_magCalibResults.beforeStdError << "\t" << m_magCalibResults.beforeMaxError << endl;
  mag_results_stream << "[After]\t" << m_magCalibResults.afterMeanError << "\t" << m_magCalibResults.afterStdError << "\t" << m_magCalibResults.afterMaxError << endl;
  mag_results_stream << "[Accuracy]\t" << sbgRadToDegF(m_magCalibResults.meanAccuracy) << "\t" << sbgRadToDegF(m_magCalibResults.stdAccuracy) << "\t" << sbgRadToDegF(m_magCalibResults.maxAccuracy) << endl;
  mag_results_stream << "* Offset\t" << m_magCalibResults.offset[0] << "\t" << m_magCalibResults.offset[1] << "\t" << m_magCalibResults.offset[2] << endl;

  mag_results_stream << "* Matrix" << endl;
  mag_results_stream << m_magCalibResults.matrix[0] << "\t" << m_magCalibResults.matrix[1] << "\t" << m_magCalibResults.matrix[2] << endl;
  mag_results_stream << m_magCalibResults.matrix[3] << "\t" << m_magCalibResults.matrix[4] << "\t" << m_magCalibResults.matrix[5] << endl;
  mag_results_stream << m_magCalibResults.matrix[6] << "\t" << m_magCalibResults.matrix[7] << "\t" << m_magCalibResults.matrix[8] << endl;

  output_filename = "mag_calib_" + timeToStr() + ".txt";
  ofstream output_file(output_filename);
  output_file << mag_results_stream.str();
  output_file.close();

  RCLCPP_INFO(m_ref_node_.get_logger(), "%s", mag_results_stream.str().c_str());
  RCLCPP_INFO(m_ref_node_.get_logger(), "SBG DRIVER [Mag Calib] - Magnetometers calibration results saved to file %s", output_filename.c_str());
}

//---------------------------------------------------------------------//
//- Parameters                                                        -//
//---------------------------------------------------------------------//

uint32_t SbgDevice::getUpdateFrequency(void) const
{
  return m_rate_frequency_;
}

//---------------------------------------------------------------------//
//- Public  methods                                                   -//
//---------------------------------------------------------------------//

void SbgDevice::initDeviceForReceivingData(void)
{
  SbgErrorCode error_code;
  initPublishers();
  configure();

  error_code = sbgEComSetReceiveLogCallback(&m_com_handle_, onLogReceivedCallback, this);

  if (error_code != SBG_NO_ERROR)
  {
    rclcpp::exceptions::throw_from_rcl_error(RCL_RET_ERROR, "SBG_DRIVER - [Init] Unable to set the callback function - " + std::string(sbgErrorCodeToString(error_code)));
  }
}

void SbgDevice::initDeviceForMagCalibration(void)
{
  m_calib_service_      = m_ref_node_.create_service<std_srvs::srv::Trigger>("sbg/mag_calibration", std::bind(&SbgDevice::processMagCalibration, this, std::placeholders::_1, std::placeholders::_2));
  m_calib_save_service_ = m_ref_node_.create_service<std_srvs::srv::Trigger>("sbg/mag_calibration_save", std::bind(&SbgDevice::saveMagCalibration, this, std::placeholders::_1, std::placeholders::_2));

  RCLCPP_INFO(m_ref_node_.get_logger(), "SBG DRIVER [Init] - SBG device is initialized for magnetometers calibration.");
}

void SbgDevice::periodicHandle(void)
{
  sbgEComHandle(&m_com_handle_);
}
