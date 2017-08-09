#include "ensenso.hpp"
#include "eigen.hpp"
#include "util.hpp"
#include "opencv.hpp"
#include "pcl.hpp"

#include <stdexcept>

namespace dr {

Ensenso::Ensenso(std::string serial, bool connect_monocular) {
	// Initialize nxLib.
	nxLibInitialize();

	if (serial == "") {
		// Try to find a stereo camera.
		boost::optional<NxLibItem> camera = openCameraByType(valStereo);
		if (!camera) throw std::runtime_error("Please connect an Ensenso stereo camera to your computer.");
		ensenso_camera = *camera;
	} else {
		// Open the requested camera.
		boost::optional<NxLibItem> camera = openCameraBySerial(serial);
		if (!camera) throw std::runtime_error("Could not find an Ensenso camera with serial " + serial);
		ensenso_camera = *camera;
	}

	// Get the linked monocular camera.
	if (connect_monocular) monocular_camera = openCameraByLink(serialNumber());
}

Ensenso::~Ensenso() {
	executeNx(NxLibCommand(cmdClose));
	nxLibFinalize();
}

std::string Ensenso::serialNumber() const {
	return getNx<std::string>(ensenso_camera[itmSerialNumber]);
}

std::string Ensenso::monocularSerialNumber() const {
	return monocular_camera ? getNx<std::string>(monocular_camera.get()[itmSerialNumber]) : "";
}

bool Ensenso::loadParameters(std::string const parameters_file) {
	bool ret = setNxJsonFromFile(ensenso_camera[itmParameters], parameters_file);

	std::ofstream file("params.json");
	if (file.is_open())
		file << ensenso_camera[itmParameters].asJson(true); // write entire camera node content as JSON with pretty printing into text file
	file.close();

	return ret;
}

bool Ensenso::loadMonocularParameters(std::string const parameters_file) {
	if (!monocular_camera) throw std::runtime_error("No monocular camera found. Can not load monocular camara parameters.");
	return setNxJsonFromFile(monocular_camera.get()[itmParameters], parameters_file);
}

void Ensenso::loadMonocularUeyeParameters(std::string const parameters_file) {
	if (!monocular_camera) throw std::runtime_error("No monocular camera found. Can not load monocular camera UEye parameters.");
	NxLibCommand command(cmdLoadUEyeParameterSet);
	setNx(command.parameters()[itmFilename], parameters_file);
	executeNx(command);
}

int Ensenso::flexView() const {
	try {
		// in case FlexView = false, getting the int value gives an error
		return getNx<int>(ensenso_camera[itmParameters][itmCapture][itmFlexView]);
	} catch (NxError const & e) {
		return -1;
	}
}

void Ensenso::setFlexView(int value) {
	setNx(ensenso_camera[itmParameters][itmCapture][itmFlexView], value);
}

void Ensenso::setFrontLight(bool state) {
	setNx(ensenso_camera[itmParameters][itmCapture][itmFrontLight], state);
}

void Ensenso::setProjector(bool state) {
	setNx(ensenso_camera[itmParameters][itmCapture][itmProjector], state);
}

bool Ensenso::trigger(bool stereo, bool monocular) const {
	monocular = monocular && monocular_camera;

	NxLibCommand command(cmdTrigger);
	if (stereo) setNx(command.parameters()[itmCameras][0], serialNumber());
	if (monocular) setNx(command.parameters()[itmCameras][stereo ? 1 : 0], getNx<std::string>(monocular_camera.get()[itmSerialNumber]));
	executeNx(command);

	if (stereo && !getNx<bool>(command.result()[serialNumber()][itmTriggered])) return false;
	if (monocular && !getNx<bool>(command.result()[monocularSerialNumber()][itmTriggered])) return false;
	return true;
}

bool Ensenso::retrieve(bool trigger, unsigned int timeout, bool stereo, bool monocular) const {
	monocular = monocular && monocular_camera;

	// nothing to do?
	if (!stereo && !monocular)
		return true;

	NxLibCommand command(trigger ? cmdCapture : cmdRetrieve);
	setNx(command.parameters()[itmTimeout], int(timeout));
	if (stereo) setNx(command.parameters()[itmCameras][0], serialNumber());
	if (monocular) setNx(command.parameters()[itmCameras][stereo ? 1 : 0], getNx<std::string>(monocular_camera.get()[itmSerialNumber]));
	executeNx(command);

	if (stereo && !getNx<bool>(command.result()[serialNumber()][itmRetrieved])) return false;
	if (monocular && !getNx<bool>(command.result()[monocularSerialNumber()][itmRetrieved])) return false;
	return true;
}

void Ensenso::rectifyImages() {
	NxLibCommand command(cmdRectifyImages);
	setNx(command.parameters()[itmCameras][0], serialNumber());
	executeNx(command);
}

cv::Size Ensenso::getIntensitySize() {
	int width, height;

	try {
		monocular_camera.get()[itmImages][itmRaw].getBinaryDataInfo(&width, &height, 0, 0, 0, 0);
	} catch (NxLibException const & e) {
		throw NxError(e);
	}

	return cv::Size(width, height);
}

cv::Size Ensenso::getPointCloudSize() {
	int width, height;
	ensenso_camera[itmImages][itmPointMap].getBinaryDataInfo(&width, &height, 0, 0, 0, 0);
	return cv::Size(width, height);
}

void Ensenso::loadIntensity(cv::Mat & intensity, bool capture) {
	if (capture) this->retrieve(true, 1500, !monocular_camera, !!monocular_camera);

	// Copy to cv::Mat.
	if (monocular_camera) {
		intensity = toCvMat(monocular_camera.get()[itmImages][itmRaw]);
	} else {
		rectifyImages();
		intensity = toCvMat(ensenso_camera[itmImages][itmRectified][itmLeft]);
	}
}

void Ensenso::loadPointCloud(pcl::PointCloud<pcl::PointXYZ> & cloud, cv::Rect roi, bool capture) {
	// Optionally capture new data.
	if (capture) this->retrieve();

	setRegionOfInterest(roi);
	std::string serial = serialNumber();

	// Compute disparity.
	{
		NxLibCommand command(cmdComputeDisparityMap);
		setNx(command.parameters()[itmCameras], serial);
		executeNx(command);
	}

	// Compute point cloud.
	{
		NxLibCommand command(cmdComputePointMap);
		setNx(command.parameters()[itmCameras], serial);
		executeNx(command);
	}

	// Convert the binary data to a point cloud.
	cloud = toPointCloud(ensenso_camera[itmImages][itmPointMap]);
}

void Ensenso::loadRegisteredPointCloud(pcl::PointCloud<pcl::PointXYZ> & cloud, cv::Rect roi, bool capture) {
	// Optionally capture new data.
	if (capture) this->retrieve();

	setRegionOfInterest(roi);
	std::string serial = serialNumber();

	// Compute disparity.
	{
		NxLibCommand command(cmdComputeDisparityMap);
		setNx(command.parameters()[itmCameras], serial);
		executeNx(command);
	}

	// Render point cloud.
	{
		NxLibCommand command(cmdRenderPointMap);
		setNx(command.parameters()[itmNear], 1); // distance in millimeters to the camera (clip nothing?)
		setNx(command.parameters()[itmCamera], monocularSerialNumber());
		// gives weird (RenderPointMap) results with OpenGL enabled, so disable
		setNx(root[itmParameters][itmRenderPointMap][itmUseOpenGL], false);
		executeNx(command);
	}

	// Convert the binary data to a point cloud.
	cloud = toPointCloud(root[itmImages][itmRenderPointMap]);
}

void Ensenso::setRegionOfInterest(cv::Rect const & roi) {
	if (roi.area() == 0) {
		setNx(ensenso_camera[itmParameters][itmCapture][itmUseDisparityMapAreaOfInterest], false);

		if (ensenso_camera[itmParameters][itmDisparityMap][itmAreaOfInterest].exists()) {
			ensenso_camera[itmParameters][itmDisparityMap][itmAreaOfInterest].erase();
		}
	} else {
		setNx(ensenso_camera[itmParameters][itmCapture][itmUseDisparityMapAreaOfInterest],          true);
		setNx(ensenso_camera[itmParameters][itmDisparityMap][itmAreaOfInterest][itmLeftTop][0],     roi.tl().x);
		setNx(ensenso_camera[itmParameters][itmDisparityMap][itmAreaOfInterest][itmLeftTop][1],     roi.tl().y);
		setNx(ensenso_camera[itmParameters][itmDisparityMap][itmAreaOfInterest][itmRightBottom][0], roi.br().x);
		setNx(ensenso_camera[itmParameters][itmDisparityMap][itmAreaOfInterest][itmRightBottom][1], roi.br().y);
	}
}

void Ensenso::discardCalibrationPatterns() {
	executeNx(NxLibCommand(cmdDiscardPatterns));
}

void Ensenso::recordCalibrationPattern() {
	// disable FlexView
	int flex_view = flexView();
	if (flex_view > 0) setFlexView(0);

	// Capture image with front-light.
	setProjector(false);
	setFrontLight(true);

	retrieve(true, 1500, true, false);

	setFrontLight(false);
	setProjector(true);

	// Find the pattern.
	NxLibCommand command_collect_pattern(cmdCollectPattern);
	setNx(command_collect_pattern.parameters()[itmCameras], serialNumber());
	setNx(command_collect_pattern.parameters()[itmDecodeData], true);

	executeNx(command_collect_pattern);

	// restore FlexView setting
	if (flex_view > 0) {
		setFlexView(flex_view);
	}
}

Eigen::Isometry3d Ensenso::detectCalibrationPattern(int const samples, bool ignore_calibration)  {
	discardCalibrationPatterns();

	for (int i = 0; i < samples; ++i) {
		recordCalibrationPattern();
	}

	// Disable FlexView (should not be necessary here, but appears to be necessary for cmdEstimatePatternPose)
	int flex_view = flexView();
	if (flex_view > 0) setFlexView(0);

	// Get the pose of the pattern.
	NxLibCommand command_estimate_pose(cmdEstimatePatternPose);
	executeNx(command_estimate_pose);

	// Restore FlexView setting.
	if (flex_view > 0) {
		setFlexView(flex_view);
	}

	Eigen::Isometry3d result = toEigenIsometry(command_estimate_pose.result()["Patterns"][0][itmPatternPose]);
	result.translation() *= 0.001;

	// Transform back to left stereo lens.
	if (ignore_calibration) {
		boost::optional<Eigen::Isometry3d> camera_pose = getWorkspaceCalibration();
		if (camera_pose) {
			result = *camera_pose * result;
		}
	}

	return result;
}

std::string Ensenso::getWorkspaceCalibrationFrame() {
	// Make sure the relevant nxLibItem exists and is non-empty, then return it.
	NxLibItem item = ensenso_camera[itmLink][itmTarget];
	int error;
	std::string result = item.asString(&error);
	return error ? "" : result;
}

boost::optional<Eigen::Isometry3d> Ensenso::getWorkspaceCalibration() {
	// Check if the camera is calibrated.
	if (getWorkspaceCalibrationFrame().empty()) return boost::none;

	// convert from mm to m
	Eigen::Isometry3d pose = toEigenIsometry(ensenso_camera[itmLink]);
	pose.translation() *= 0.001;

	return pose;
}

Ensenso::CalibrationResult Ensenso::computeCalibration(
	std::vector<Eigen::Isometry3d> const & robot_poses,
	bool moving,
	boost::optional<Eigen::Isometry3d> const & camera_guess,
	boost::optional<Eigen::Isometry3d> const & pattern_guess,
	std::string const & target
) {
	NxLibCommand calibrate(cmdCalibrateHandEye);

	// camera pose initial guess
	if (camera_guess) {
		Eigen::Isometry3d scaled_camera_guess = *camera_guess;
		scaled_camera_guess.translation() *= 1000;
		setNx(calibrate.parameters()[itmLink], scaled_camera_guess);
	}

	// pattern pose initial guess
	if (pattern_guess) {
		Eigen::Isometry3d scaled_pattern_guess = *pattern_guess;
		scaled_pattern_guess.translation() *= 1000;
		setNx(calibrate.parameters()[itmPatternPose], scaled_pattern_guess);
	}

	// setup (camera in hand / camera fixed)
	setNx(calibrate.parameters()[itmSetup], moving ? valMoving : valFixed);

	// name of the target coordinate system
	if (target != "") {
		setNx(calibrate.parameters()[itmTarget], target);
	}

	// copy robot poses to parameters
	for (size_t i = 0; i < robot_poses.size(); i++) {
		Eigen::Isometry3d scaled_robot_pose = robot_poses[i];
		scaled_robot_pose.translation() *= 1000;
		setNx(calibrate.parameters()[itmTransformations][i], scaled_robot_pose);
	}

	// execute calibration command
	executeNx(calibrate);

	// return result (camera pose, pattern pose, iterations, reprojection error)
	Eigen::Isometry3d camera_pose  = toEigenIsometry(ensenso_camera[itmLink]).inverse(); // "Link" is inverted
	Eigen::Isometry3d pattern_pose = toEigenIsometry(calibrate.result()[itmPatternPose]);
	camera_pose.translation()  *= 0.001;
	pattern_pose.translation() *= 0.001;

	return Ensenso::CalibrationResult{
		camera_pose,
		pattern_pose,
		getNx<int>(calibrate.result()[itmIterations]),
		getNx<double>(calibrate.result()[itmReprojectionError])
	};
}

void Ensenso::setWorkspaceCalibration(Eigen::Isometry3d const & workspace, std::string const & frame_id, Eigen::Isometry3d const & defined_pose, bool store) {
	NxLibCommand command(cmdCalibrateWorkspace);
	setNx(command.parameters()[itmCameras][0], serialNumber());

	// scale to [mm]
	Eigen::Isometry3d workspace_mm = workspace;
	workspace_mm.translation() *= 1000;
	setNx(command.parameters()[itmPatternPose], workspace_mm);

	if (frame_id != "") {
		setNx(command.parameters()[itmTarget], frame_id);
	}

	Eigen::Isometry3d defined_pose_mm = defined_pose;
	defined_pose_mm.translation() *= 1000;
	setNx(command.parameters()[itmDefinedPose], defined_pose_mm);

	executeNx(command);

	if (store) storeWorkspaceCalibration();
}

void Ensenso::clearWorkspaceCalibration(bool store) {
	// Check if the camera is calibrated.
	if (getWorkspaceCalibrationFrame().empty()) return;

	// calling CalibrateWorkspace with no PatternPose and DefinedPose clears the workspace.
	NxLibCommand command(cmdCalibrateWorkspace);
	setNx(command.parameters()[itmCameras][0], serialNumber());
	setNx(command.parameters()[itmTarget], "");
	executeNx(command);

	// clear target name
	// TODO: Can be removed after settings the target parameter above?
	// TODO: Should test that.
	setNx(ensenso_camera[itmLink][itmTarget], serialNumber() + "_frame");

	if (store) storeWorkspaceCalibration();
}

void Ensenso::storeWorkspaceCalibration() {
	NxLibCommand command(cmdStoreCalibration);
	setNx(command.parameters()[itmCameras][0], serialNumber());

	// Override calibration in EEPROM.
	setNx(command.parameters()[itmCalibration][0], true);

	// Store and override the target link in EEPROM.
	setNx(command.parameters()[itmLink][0], true);
	executeNx(command);
}

}
