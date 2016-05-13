#include <lwr_pick_n_place/pick_n_place.hpp>

PickNPlace::PickNPlace() : 
  spinner_(1)
{
  // Start AsyncSpinner
  spinner_.start();
  
  // Get params
  double max_planning_time;
  ros::NodeHandle nh, nh_param("~");
  nh_param.param<std::string>("base_frame", base_frame_ , "base_link");
  nh_param.param<std::string>("ee_frame", ee_frame_, "link_7");
  nh_param.param<std::string>("group_name", group_name_, "arm");
  nh_param.param<double>("max_planning_time", max_planning_time, 8.0);
  nh_param.param<double>("gripping_offset", gripping_offset_, 0.1);
  nh_param.param<double>("dz_offset", dz_offset_, 0.3);
  
  // Initialize move group
  group_.reset(new move_group_interface::MoveGroup(group_name_));
  group_->setPlanningTime(max_planning_time);
  group_->allowReplanning(false);
  // TODO What is this 1.0 exactly ?
  group_->startStateMonitor(1.0);
  group_->setPlannerId("RRTConnectkConfigDefault");
  group_->setEndEffectorLink(ee_frame_);
  group_->setPoseReferenceFrame(ee_frame_);
  group_->setGoalPositionTolerance(0.001);
  group_->setGoalOrientationTolerance(0.001);

  // Configure service calls
  fk_srv_req_.header.frame_id = base_frame_;
  fk_srv_req_.fk_link_names.push_back(ee_frame_);
  ik_srv_req_.ik_request.group_name = group_name_;
  ik_srv_req_.ik_request.pose_stamped.header.frame_id = base_frame_;
  ik_srv_req_.ik_request.attempts = 100;
  ik_srv_req_.ik_request.timeout = ros::Duration(0.1);
  ik_srv_req_.ik_request.ik_link_name = ee_frame_;
  ik_srv_req_.ik_request.ik_link_names.push_back(ee_frame_);
  ik_srv_req_.ik_request.avoid_collisions = true;
  cart_path_srv_req_.group_name = group_name_;
  cart_path_srv_req_.header.frame_id = base_frame_;
  cart_path_srv_req_.max_step = 0.05;
  cart_path_srv_req_.jump_threshold = 0.0;
  cart_path_srv_req_.avoid_collisions = true;
  cart_path_srv_req_.link_name = ee_frame_;
  
  // Initialize planning scene monitor
  tf_.reset(new tf::TransformListener(ros::Duration(2.0)));
  planning_scene_monitor_.reset(new planning_scene_monitor::PlanningSceneMonitor("robot_description", tf_));
  planning_scene_monitor_->startSceneMonitor();
  planning_scene_monitor_->startStateMonitor();
  planning_scene_monitor_->startWorldGeometryMonitor();
  
  // Wait until the required ROS services are available
  ik_service_client_ = nh.serviceClient<moveit_msgs::GetPositionIK> ("compute_ik");
  fk_service_client_ = nh.serviceClient<moveit_msgs::GetPositionFK> ("compute_fk");
  cartesian_path_service_client_ = nh.serviceClient<moveit_msgs::GetCartesianPath>(move_group::CARTESIAN_PATH_SERVICE_NAME);
  while(!ik_service_client_.exists() || !fk_service_client_.exists() || !cartesian_path_service_client_.exists() )
  {
    ROS_INFO("Waiting for service");
    sleep(1.0);
  }

  // Wait for subscribers to make sure we can publish attached/unattached objects //
  attached_object_publisher_ = nh.advertise<moveit_msgs::AttachedCollisionObject>("attached_collision_object", 1);
  planning_scene_diff_publisher_ = nh.advertise<moveit_msgs::PlanningScene>("planning_scene", 1);
  while(attached_object_publisher_.getNumSubscribers() < 1 || planning_scene_diff_publisher_.getNumSubscribers() < 1)
  {
    ROS_INFO("Waiting for planning scene");
    sleep(1.0);
  }
  
  // Add some extra sleep to make sure the planning scene is loaded
  usleep(1000000*3);
}

void PickNPlace::getPlanningScene(moveit_msgs::PlanningScene& planning_scene, planning_scene::PlanningScenePtr& full_planning_scene)
{
  planning_scene_monitor_->requestPlanningSceneState();
  full_planning_scene = planning_scene_monitor_->getPlanningScene();
  full_planning_scene->getPlanningSceneMsg(planning_scene);
}

bool PickNPlace::compute_fk(const sensor_msgs::JointState joints, geometry_msgs::Pose &pose)
{
  // Update planning scene and robot state
//   getPlanningScene(planning_scene_msg_, full_planning_scene_);
  
  fk_srv_req_.header.stamp = ros::Time::now();
  fk_srv_req_.robot_state = planning_scene_msg_.robot_state;
  fk_srv_req_.robot_state.joint_state = joints;
  fk_service_client_.call(fk_srv_req_, fk_srv_resp_);
  
  if(fk_srv_resp_.error_code.val !=1){
    ROS_ERROR("FK couldn't find a solution (error code %d)", fk_srv_resp_.error_code.val);
    return false;
  }
  
  ROS_INFO("ee_frame has pose (%.2f, %.2f, %.2f)", fk_srv_resp_.pose_stamped[0].pose.position.x, fk_srv_resp_.pose_stamped[0].pose.position.y, fk_srv_resp_.pose_stamped[0].pose.position.z);
  pose = fk_srv_resp_.pose_stamped[0].pose;
  return true;
}

bool PickNPlace::compute_ik(const geometry_msgs::Pose pose, sensor_msgs::JointState &joints)
{
  // Update planning scene and robot state
//   getPlanningScene(planning_scene_msg_, full_planning_scene_);
//   group_->getCurrentState()->update(true);
  
//   std::vector<double> test_joints;
//   this->getCurrentJointPosition(test_joints);
  
  // setup IK request
//   ik_srv_req_.ik_request.robot_state = planning_scene_msg_.robot_state;
  ik_srv_req_.ik_request.pose_stamped.header.stamp = ros::Time::now();
  ik_srv_req_.ik_request.pose_stamped.pose = pose;
  
  ik_service_client_.call(ik_srv_req_, ik_srv_resp_);
  if(ik_srv_resp_.error_code.val !=1){
    ROS_ERROR("IK couldn't find a solution (error code %d)", ik_srv_resp_.error_code.val);
    return false;
  }
  ROS_INFO("IK returned succesfully");

  joints = ik_srv_resp_.solution.joint_state;
  
//   this->IKCorrection(joints);
  
  return true;
}

bool PickNPlace::getCurrentCartesianPose(geometry_msgs::Pose &pose, std::string target_frame)
{
  // Update planning scene and robot state
  getPlanningScene(planning_scene_msg_, full_planning_scene_);
  
  // Call IK with current joint state
//   return compute_fk(planning_scene_msg_.robot_state.joint_state, pose);
  pose = group_->getCurrentPose(target_frame).pose;
  return true;
}

bool PickNPlace::getCurrentJointPosition(std::vector<double> &joints)
{
  // Update planning scene and robot state
  getPlanningScene(planning_scene_msg_, full_planning_scene_);
  
  joints = planning_scene_msg_.robot_state.joint_state.position;
  
  for (int i=0; i<joints.size() ;i++){
    ROS_WARN_STREAM("Joint "<<i<<" is : "<<joints[i]);
  }
  
  return true;
}

bool PickNPlace::executeJointTrajectory(const MoveGroupPlan mg_plan)
{
  int num_pts = mg_plan.trajectory_.joint_trajectory.points.size();
  ROS_INFO("Executing joint trajectory with %d knots and duration %f", num_pts, 
      mg_plan.trajectory_.joint_trajectory.points[num_pts-1].time_from_start.toSec());
  
  return group_->execute(mg_plan);
}

void PickNPlace::stopJointTrajectory()
{
  ROS_INFO("Stopping current joint trajectory");
  group_->stop();
}

bool PickNPlace::moveToJointPosition(const std::vector<double> joint_vals)
{
//   getPlanningScene(planning_scene_msg_, full_planning_scene_);
//   group_->getCurrentState()->update(true);
  
  // Set joint target
  group_->setJointValueTarget(joint_vals);

  // Plan trajectory
  if (!group_->plan(next_plan_)){
      ROS_INFO("Motion planning to joint position failed");
    return false;
  }
  ROS_INFO("Motion planning to joint position successful");

  // Execute trajectory
  if (executeJointTrajectory(next_plan_)) {
    ROS_INFO("Trajectory execution successful");
    return true;
  }
  else {
    ROS_ERROR("Trajectory execution failed");
    return false;
  }
}

bool PickNPlace::moveToCartesianPose(const geometry_msgs::Pose pose)
{
  
//   getPlanningScene(planning_scene_msg_, full_planning_scene_);
//   group_->getCurrentState()->update(true);
  
  // Compute ik
  sensor_msgs::JointState joints_ik;
  if (!compute_ik(pose, joints_ik))
    return false;

  // Set joint target
  group_->setJointValueTarget(joints_ik);

  // Plan trajectory
  if (!group_->plan(next_plan_)){
      ROS_INFO("Motion planning to position (%.2f, %.2f, %.2f) failed", 
      pose.position.x, pose.position.y, pose.position.z);
    return false;
  }
  ROS_INFO("Motion planning to position (%.2f, %.2f, %.2f) successful", 
      pose.position.x, pose.position.y, pose.position.z);

  // Execute trajectory
  if (executeJointTrajectory(next_plan_)) {
    ROS_INFO("Trajectory execution successful");
    return true;
  }
  else {
    ROS_ERROR("Trajectory execution failed");
    return false;
  }
}

bool PickNPlace::moveToStart()
{
  group_->setNamedTarget("start");
  
  // Plan trajectory
  if (!group_->plan(next_plan_)){
    ROS_INFO("Home position motion planning failed");
    return false;
  }
  ROS_INFO("Home position motion planning successful");

  // Execute trajectory
  if (executeJointTrajectory(next_plan_)) {
    ROS_INFO("Home position joint trajectory execution successful");
    return true;
  }
  else {
    ROS_ERROR("Home position joint trajectory execution failed");
    return false;
  }
}

bool PickNPlace::moveToRandomTarget()
{
  group_->setRandomTarget();
  
  // Plan trajectory
  if (!group_->plan(next_plan_)){
    ROS_INFO("Motion planning to random target failed");
    return false;
  }
  ROS_INFO("Motion planning to random target successful");

  // Execute trajectory
  if (executeJointTrajectory(next_plan_)) {
    ROS_INFO("Joint trajectory execution to random target successful");
    return true;
  }
  else {
    ROS_ERROR("Joint trajectory execution to random target failed");
    return false;
  }
}

bool PickNPlace::getCollisionObject(const std::string obj_name, moveit_msgs::CollisionObject &object)
{
  // Update planning scene
  getPlanningScene(planning_scene_msg_, full_planning_scene_);

  for(int i=0;i<planning_scene_msg_.world.collision_objects.size();i++){
    if(planning_scene_msg_.world.collision_objects[i].id == obj_name){
      object = planning_scene_msg_.world.collision_objects[i];
      ROS_INFO_STREAM("Found object "<< obj_name <<" in the planning scene");
      return true;
    }
  }
  ROS_ERROR_STREAM("Failed to find object "<< obj_name <<" in the planning scene");
  return false;
}

bool PickNPlace::verticalMove(double target_z)
{
  ROS_INFO("Vertical move to target z: %f", target_z);

  // Set two waypoints for the linear trajectory
  geometry_msgs::Pose pose1, pose2;
  getCurrentCartesianPose(pose1);
  pose2 = pose1;
  ROS_INFO("Calling cart path from pose pos = (%.2f, %.2f, %.2f), quat = (%.2f, %.2f, %.2f, w %.2f)",
      pose1.position.x, pose1.position.y, pose1.position.z, pose1.orientation.x, pose1.orientation.y, pose1.orientation.z, pose1.orientation.w);
  pose1.position.z = (pose1.position.z+target_z)/2.0;
  pose2.position.z = target_z;
  ROS_INFO("for pose pos = (%.2f, %.2f, %.2f), quat = (%.2f, %.2f, %.2f, w %.2f)",
      pose2.position.x, pose2.position.y, pose2.position.z, pose2.orientation.x, pose2.orientation.y, pose2.orientation.z, pose2.orientation.w);
  
  // Find linear trajectory
  moveit_msgs::RobotTrajectory lin_traj_msg;
  std::vector<geometry_msgs::Pose> waypoints;
//   waypoints.push_back(pose1);
  waypoints.push_back(pose2);

  // Call cartesian path service
  cart_path_srv_req_.header.stamp = ros::Time::now();
  cart_path_srv_req_.waypoints = waypoints;
  robot_state::robotStateToRobotStateMsg(*group_->getCurrentState(), cart_path_srv_req_.start_state);
  cartesian_path_service_client_.call(cart_path_srv_req_,cart_path_srv_resp_);
  if (cart_path_srv_resp_.error_code.val != 1) {
    ROS_ERROR("Cartesian path service returned with error code %d", cart_path_srv_resp_.error_code.val);
    return false;
  }
  if(cart_path_srv_resp_.fraction < 0.0) {
    ROS_ERROR("Failed compute a correct CartesianPath");
    return false;
  }

  // Execute plan
  MoveGroupPlan lin_traj_plan;
  lin_traj_plan.trajectory_ = cart_path_srv_resp_.solution;
  if (executeJointTrajectory(lin_traj_plan)) {
    ROS_INFO("Vertical joint trajectory execution successful");
    return true;
  }
  else {
    ROS_ERROR("Vertical joint trajectory execution failed, going to restart");
    return false;
  }
}

bool PickNPlace::verticalMoveBis(double target_z)
{
  ROS_INFO("Vertical move to target z: %f", target_z);

  // Update planning scene and robot state
  getPlanningScene(planning_scene_msg_, full_planning_scene_);

  // Set two waypoints for the linear trajectory
  geometry_msgs::Pose pose = group_->getCurrentPose(ee_frame_).pose;
  pose.position.z = target_z;
  
  moveit_msgs::Constraints constraints;
  moveit_msgs::OrientationConstraint ocm;
  ocm.header.frame_id = base_frame_;
  ocm.header.stamp = ros::Time::now();
  ocm.orientation = pose.orientation;
  ocm.link_name = ee_frame_;
  ocm.absolute_x_axis_tolerance = 0.5;
  ocm.absolute_y_axis_tolerance = 0.5;
  ocm.absolute_z_axis_tolerance = 3.14;
  ocm.weight = 1.0;
  constraints.orientation_constraints.push_back(ocm);
  group_->setPathConstraints(constraints);
  
  // Compute ik
  sensor_msgs::JointState joints_ik;
  if (!compute_ik(pose, joints_ik))
    return false;
  
  // Set joint target
  group_->setJointValueTarget(joints_ik);
  
    // Plan trajectory
  if (!group_->plan(next_plan_)){
    group_->clearPathConstraints();
      ROS_INFO("Motion planning to position (%.2f, %.2f, %.2f) failed", 
      pose.position.x, pose.position.y, pose.position.z);
    return false;
  }
  group_->clearPathConstraints();
  ROS_INFO("Motion planning to position (%.2f, %.2f, %.2f) successful", 
      pose.position.x, pose.position.y, pose.position.z);

  // Execute trajectory
  if (executeJointTrajectory(next_plan_)) {
    ROS_INFO("Trajectory execution successful");
    return true;
  }
  else {
    ROS_ERROR("Trajectory execution failed");
    return false;
  }
  
}

bool PickNPlace::addCylinderObject(const geometry_msgs::Pose object_pose)
{
  getPlanningScene(planning_scene_msg_, full_planning_scene_);
  
  moveit_msgs::CollisionObject collision_object;
  collision_object.id = "cylinder";
  collision_object.header.frame_id = "base_link";
  collision_object.header.stamp = ros::Time::now();
  collision_object.operation = moveit_msgs::CollisionObject::ADD;
  
  // Define the collision object as a cylinder
  shape_msgs::SolidPrimitive primitive_object;
  primitive_object.type = shape_msgs::SolidPrimitive::CYLINDER;
  primitive_object.dimensions.push_back(0.13); // height
  primitive_object.dimensions.push_back(0.015); // radius
  collision_object.primitives.push_back(primitive_object);
  collision_object.primitive_poses.push_back(object_pose);
  
  // Define the collision object as a mesh
//   collision_object.meshes.clear();
//   collision_object.mesh_poses.clear();
//   collision_object.meshes.push_back(co_mesh);
//   collision_object.mesh_poses.push_back(mesh_pose);

  // Put the object in the environment //
  planning_scene_msg_.world.collision_objects.clear();
  planning_scene_msg_.world.collision_objects.push_back(collision_object);
  planning_scene_msg_.is_diff = true;
  planning_scene_diff_publisher_.publish(planning_scene_msg_);
    
  return true;
}


bool PickNPlace::addBoxObject(const geometry_msgs::Pose object_pose)
{
  getPlanningScene(planning_scene_msg_, full_planning_scene_);
  
  moveit_msgs::CollisionObject collision_object;
  collision_object.id = "box";
  collision_object.header.frame_id = "base_link";
  collision_object.header.stamp = ros::Time::now();
  collision_object.operation = moveit_msgs::CollisionObject::ADD;
  
  // Define the collision object as a cylinder
  shape_msgs::SolidPrimitive primitive_object;
  primitive_object.type = shape_msgs::SolidPrimitive::BOX;
  primitive_object.dimensions.push_back(0.5); 
  primitive_object.dimensions.push_back(0.5);
  primitive_object.dimensions.push_back(0.5);
  collision_object.primitives.push_back(primitive_object);
  collision_object.primitive_poses.push_back(object_pose);

  // Put the object in the environment //
  planning_scene_msg_.world.collision_objects.clear();
  planning_scene_msg_.world.collision_objects.push_back(collision_object);
  planning_scene_msg_.is_diff = true;
  planning_scene_diff_publisher_.publish(planning_scene_msg_);
    
  return true;
}

bool PickNPlace::addEpingleObject(const geometry_msgs::Pose object_pose)
{
  getPlanningScene(planning_scene_msg_, full_planning_scene_);
  
  moveit_msgs::CollisionObject collision_object;
  collision_object.id = "epingle";
  collision_object.header.frame_id = "base_link";
  collision_object.header.stamp = ros::Time::now();
  collision_object.operation = moveit_msgs::CollisionObject::ADD;
  
  // Define the collision object as a mesh
  shapes::Mesh* m;
  shape_msgs::Mesh co_mesh;
  shapes::ShapeMsg co_mesh_msg;  
  m = shapes::createMeshFromResource("package://lwr_pick_n_place/meshes/epingle.stl");
  shapes::constructMsgFromShape(m,co_mesh_msg);
  co_mesh = boost::get<shape_msgs::Mesh>(co_mesh_msg);
  collision_object.meshes.clear();
  collision_object.mesh_poses.clear();
  collision_object.meshes.push_back(co_mesh);
  collision_object.mesh_poses.push_back(object_pose);

  // Put the object in the environment //
  planning_scene_msg_.world.collision_objects.clear();
  planning_scene_msg_.world.collision_objects.push_back(collision_object);
  planning_scene_msg_.is_diff = true;
  planning_scene_diff_publisher_.publish(planning_scene_msg_);
    
  return true;
}

bool PickNPlace::addPlaqueObject(const geometry_msgs::Pose object_pose)
{
  getPlanningScene(planning_scene_msg_, full_planning_scene_);
  
  moveit_msgs::CollisionObject collision_object;
  collision_object.id = "plaque";
  collision_object.header.frame_id = "base_link";
  collision_object.header.stamp = ros::Time::now();
  collision_object.operation = moveit_msgs::CollisionObject::ADD;
  
  // Define the collision object as a mesh
  shapes::Mesh* m;
  shape_msgs::Mesh co_mesh;
  shapes::ShapeMsg co_mesh_msg;  
  m = shapes::createMeshFromResource("package://lwr_pick_n_place/meshes/plaque.stl");
  shapes::constructMsgFromShape(m,co_mesh_msg);
  co_mesh = boost::get<shape_msgs::Mesh>(co_mesh_msg);
  collision_object.meshes.clear();
  collision_object.mesh_poses.clear();
  collision_object.meshes.push_back(co_mesh);
  collision_object.mesh_poses.push_back(object_pose);

  // Put the object in the environment //
  planning_scene_msg_.world.collision_objects.clear();
  planning_scene_msg_.world.collision_objects.push_back(collision_object);
  planning_scene_msg_.is_diff = true;
  planning_scene_diff_publisher_.publish(planning_scene_msg_);
    
  return true;
}


moveit_msgs::CollisionObjectPtr PickNPlace::getCollisionObject(std::string object_name)
{
  // update the planning scene to get the robot's state
  getPlanningScene(planning_scene_msg_, full_planning_scene_);

  for(int i=0;i<planning_scene_msg_.world.collision_objects.size();i++){
    if(planning_scene_msg_.world.collision_objects[i].id == object_name){ 
      return moveit_msgs::CollisionObjectPtr(new moveit_msgs::CollisionObject(planning_scene_msg_.world.collision_objects[i]));
    }
  }
  ROS_ERROR_STREAM("Failed to find object "<< object_name<< " in the scene !!!");
  return moveit_msgs::CollisionObjectPtr();
}

bool PickNPlace::attachObject(std::string object_name){ 

  moveit_msgs::CollisionObjectPtr coll_obj = getCollisionObject(object_name);
  if (coll_obj) {
    ROS_INFO_STREAM("Attaching object "<<object_name<<" to the end-effector");
    moveit_msgs::AttachedCollisionObject attached_object;
    attached_object.link_name = ee_frame_;
    attached_object.object = *coll_obj;
    attached_object.object.operation = attached_object.object.ADD;
    attached_object_publisher_.publish(attached_object);
    return true;
  } else 
    return false;
  
}

bool PickNPlace::detachObject(){
  ROS_INFO_STREAM("Detaching object from the robot");

  // update the planning scene to get the robot's state
  getPlanningScene(planning_scene_msg_, full_planning_scene_);

  if (planning_scene_msg_.robot_state.attached_collision_objects.size()>0){
    moveit_msgs::AttachedCollisionObject attached_object = planning_scene_msg_.robot_state.attached_collision_objects[0];
    geometry_msgs::Pose object_pose;
    this->compute_fk(planning_scene_msg_.robot_state.joint_state, object_pose);

    tf::Quaternion co_quat;
    tf::quaternionMsgToTF(object_pose.orientation, co_quat);
    double roll, pitch, yaw;
    tf::Matrix3x3(co_quat).getRPY(roll, pitch, yaw);
    tf::Quaternion quat = tf::createQuaternionFromRPY(0,0,yaw);
    
    attached_object.object.header.frame_id = "base_link";
    
    if (attached_object.object.primitive_poses.size() >0){

    attached_object.object.primitive_poses[0].position = object_pose.position;
    attached_object.object.primitive_poses[0].position.z = 0.13/2.0;
    attached_object.object.primitive_poses[0].orientation.x = quat.x();
    attached_object.object.primitive_poses[0].orientation.y = quat.y();
    attached_object.object.primitive_poses[0].orientation.z = quat.z();
    attached_object.object.primitive_poses[0].orientation.w = quat.w();
    }else{
      attached_object.object.mesh_poses[0].position = object_pose.position;
//     attached_object.object.mesh_poses[0].position.z = 0.0;
      attached_object.object.mesh_poses[0].orientation.x = quat.x();
      attached_object.object.mesh_poses[0].orientation.y = quat.y();
      attached_object.object.mesh_poses[0].orientation.z = quat.z();
      attached_object.object.mesh_poses[0].orientation.w = quat.w();
    }
    
    planning_scene_msg_.robot_state.attached_collision_objects.clear();
    planning_scene_msg_.world.collision_objects.push_back(attached_object.object);
    planning_scene_msg_.is_diff = true;
    planning_scene_diff_publisher_.publish(planning_scene_msg_);
    return true;
  }else{
    ROS_ERROR("There was no object attached to the robot");
    return false;
  }
}

void PickNPlace::cleanObjects(){
  // update the planning scene to get the robot's state
  getPlanningScene(planning_scene_msg_, full_planning_scene_);
  
//   planning_scene_msg_.robot_state.attached_collision_objects.clear();
//   planning_scene_msg_.world.collision_objects.clear();
//   planning_scene_msg_.is_diff = false;
//   planning_scene_diff_publisher_.publish(planning_scene_msg_);
//   
//   moveit_msgs::PlanningScene empty_scene;
//   empty_scene.is_diff = false;
//   planning_scene_diff_publisher_.publish(empty_scene);
  
  for (int i = 0; i<planning_scene_msg_.robot_state.attached_collision_objects.size(); i++)
    planning_scene_msg_.robot_state.attached_collision_objects[i].object.operation = moveit_msgs::CollisionObject::REMOVE;
  for (int i = 0; i<planning_scene_msg_.world.collision_objects.size(); i++)
    planning_scene_msg_.world.collision_objects[i].operation = moveit_msgs::CollisionObject::REMOVE;
  
  planning_scene_msg_.is_diff = true;
  planning_scene_diff_publisher_.publish(planning_scene_msg_);
}

bool PickNPlace::moveAboveEpingle(const std::string obj_name)
{
  ROS_INFO_STREAM("Moving above "<<obj_name);
  moveit_msgs::CollisionObjectPtr coll_obj = getCollisionObject(obj_name);
  if (!coll_obj)
    return false;

  geometry_msgs::Pose target_pose, obj_pose;
  obj_pose = coll_obj->mesh_poses[0];
  
  tf::Transform object_transform;
  object_transform.setOrigin(tf::Vector3(obj_pose.position.x, obj_pose.position.y, obj_pose.position.z));
  object_transform.setRotation(tf::Quaternion(obj_pose.orientation.x, obj_pose.orientation.y, obj_pose.orientation.z, obj_pose.orientation.w));
  
  tf::Transform up_transform;
  up_transform.setOrigin(tf::Vector3(0.0, 0.0, 0.06));
  tf::Quaternion rotation;
  rotation.setRPY(0,0,0);
  up_transform.setRotation(rotation);
  
  tf::Transform pi_rotation_transform;
  pi_rotation_transform.setOrigin(tf::Vector3(0.0, 0.0, 0.0));
  tf::Quaternion pi_rotation;
  pi_rotation.setRPY(M_PI,0,0);
  pi_rotation_transform.setRotation(pi_rotation);
  
  object_transform *= up_transform;
  object_transform *= pi_rotation_transform;
  
  target_pose.position.x = object_transform.getOrigin().getX();
  target_pose.position.y = object_transform.getOrigin().getY();
  target_pose.position.z = object_transform.getOrigin().getZ();
  target_pose.orientation.x = object_transform.getRotation().getX();
  target_pose.orientation.y = object_transform.getRotation().getY();
  target_pose.orientation.z = object_transform.getRotation().getZ();
  target_pose.orientation.w = object_transform.getRotation().getW();
  
  return this->moveToCartesianPose(target_pose);
}

bool PickNPlace::moveAbovePlaque(const std::string obj_name)
{
  ROS_INFO_STREAM("Moving above "<<obj_name);
  moveit_msgs::CollisionObjectPtr coll_obj = getCollisionObject(obj_name);
  if (!coll_obj)
    return false;

  geometry_msgs::Pose target_pose, obj_pose;
  obj_pose = coll_obj->mesh_poses[0];
  
  tf::Transform object_transform;
  object_transform.setOrigin(tf::Vector3(obj_pose.position.x, obj_pose.position.y, obj_pose.position.z));
  object_transform.setRotation(tf::Quaternion(obj_pose.orientation.x, obj_pose.orientation.y, obj_pose.orientation.z, obj_pose.orientation.w));
  
  tf::Transform up_transform;
  up_transform.setOrigin(tf::Vector3(0.0, 0.0, -0.2));
  tf::Quaternion rotation;
  rotation.setRPY(0,0,0);
  up_transform.setRotation(rotation);
  
  tf::Transform pi_rotation_transform;
  pi_rotation_transform.setOrigin(tf::Vector3(0.0, 0.0, 0.0));
  tf::Quaternion pi_rotation;
  pi_rotation.setRPY(M_PI,0,0);
  pi_rotation_transform.setRotation(pi_rotation);
  
  ROS_ERROR("object pose (%f, %f, %f)",object_transform.getOrigin().getX(), object_transform.getOrigin().getY(),object_transform.getOrigin().getZ());
  object_transform *= up_transform;
//   object_transform *= pi_rotation_transform;
  ROS_ERROR("goal pose (%f, %f, %f)",object_transform.getOrigin().getX(), object_transform.getOrigin().getY(),object_transform.getOrigin().getZ());
  
  target_pose.position.x = object_transform.getOrigin().getX();
  target_pose.position.y = object_transform.getOrigin().getY();
  target_pose.position.z = object_transform.getOrigin().getZ();
  target_pose.orientation.x = object_transform.getRotation().getX();
  target_pose.orientation.y = object_transform.getRotation().getY();
  target_pose.orientation.z = object_transform.getRotation().getZ();
  target_pose.orientation.w = object_transform.getRotation().getW();
  
  return this->moveToCartesianPose(target_pose);
}






// Not tested yet




///*
// bool PickNPlace::approachObject(int bin_number, double& bin_height)
// {
//   ROS_INFO("Approaching bin %d", bin_number);
//   if(!moveAboveObject(bin_number, bin_height)) {
//     ROS_ERROR("Failed to move above bin #%d.", bin_number);
//     return false;
//   }
//   if(!descent(bin_height)) {
//     ROS_ERROR("Failed to descend after moving above bin.");
//     return false;
//   }
//   return true;
// }
//*/


/*
bool PickNPlace::moveObjectToTarget(int bin_number, double x_target, double y_target, double angle_target, bool is_holding_bin_at_start)
{
  ROS_INFO("Moving bin %d to target (%.3f, %.3f, %f)", bin_number, x_target, y_target, angle_target);

  double bin_height;
  
  ////////////////// TYPICAL OPERATION STARTS HERE ///////////////////////////
  if (!is_holding_bin_at_start) {
//     executeGripperAction(false, false); // open gripper, but don't wait
    if(!approachObject(bin_number, bin_height)) {
      ROS_ERROR("Failed to approach bin #%d.", bin_number);
      return false;
    }
    if(!attachObject(bin_number)) {
      ROS_ERROR("Failed to attach bin #%d.", bin_number);
      return false;
    }
  }

  ///////////////////////////// HOLDING BIN ///////////////////////////////////
  if(!deliverObject(bin_number, x_target, y_target, angle_target, bin_height)) {
    ROS_ERROR("Failed to deliver bin to target (%.3f, %.3f, %f)", 
        x_target, y_target, angle_target);
    return false;
  }
  if(!detachObject()) {
    ROS_ERROR("Failed to detach bin.");
    return false;
  }
  /////////////////////////////////////////////////////////////////////////////
  if(!ascent(bin_height)) {
    ROS_ERROR("Failed to ascend after releasing bin.");
    return false;
  }
  return true;
}


bool PickNPlace::traverseMove(geometry_msgs::Pose& pose)
{
  ROS_INFO("Traverse move to position (%.2f, %.2f, %.2f)", 
      pose.position.x, pose.position.y, pose.position.z);
  while (ros::ok()) {

    // TODO
    group_->getCurrentState()->update(true);

    moveit_msgs::PlanningScene planning_scene;
    planning_scene::PlanningScenePtr full_planning_scene;
    getPlanningScene(planning_scene, full_planning_scene);

    ////////////// Perform IK to find joint goal //////////////
    moveit_msgs::GetPositionIK::Request ik_srv_req;

    // setup IK request
    ik_srv_req_.ik_request.group_name = "arm";
    ik_srv_req_.ik_request.pose_stamped.header.frame_id = "base_link";
    ik_srv_req_.ik_request.pose_stamped.header.stamp = ros::Time::now();
    ik_srv_req_.ik_request.avoid_collisions = true;
    ik_srv_req_.ik_request.attempts = 30;

    // set pose
    ik_srv_req_.ik_request.pose_stamped.pose = pose;

    moveit_msgs::GetPositionIK::Response ik_srv_resp;
    ik_service_client_.call(ik_srv_req_, ik_srv_resp);
    if(ik_srv_resp.error_code.val !=1){
      ROS_ERROR("IK couldn't find a solution (error code %d)", ik_srv_resp.error_code.val);
      return false;
    }
    ROS_INFO("IK returned succesfully");
    ///////////////////////////////////////////////////////////

    // Fixing joint_0 and joint_6 given by the IK
//     ik_srv_resp.solution.joint_state.position[0] = this->optimalGoalAngle(ik_srv_resp.solution.joint_state.position[0], planning_scene.robot_state.joint_state.position[0]);
//     ik_srv_resp.solution.joint_state.position[6] = this->optimalGoalAngle(ik_srv_resp.solution.joint_state.position[6], planning_scene.robot_state.joint_state.position[6]);

    // Plan trajectory
    group_->getCurrentState()->update(true);
    group_->setJointValueTarget(ik_srv_resp.solution.joint_state);
    int num_tries = 4;
    MoveGroupPlan my_plan;
    // try to plan a few times, just to be safe
    while (ros::ok() && num_tries > 0) {
      if (group_->plan(my_plan))
        break;
      num_tries--;
    }

    if (num_tries > 0) {
      // found plan, let's try and execute
      if (executeJointTrajectory(my_plan)) {
        ROS_INFO("Traverse joint trajectory execution successful");
        return true;
      }
      else {
        ROS_WARN("Traverse joint trajectory execution failed, going to restart");
        ros::Duration(0.5).sleep();
        continue;
      }
    }
    else {
      ROS_ERROR("Motion planning failed");
      return false;
    }
  }
}

bool PickNPlace::ascent(double bin_height)
{
  ROS_INFO("Ascending");
  return verticalMove(gripping_offset_ + bin_height + dz_offset_);
}

bool PickNPlace::descent(double bin_height)
{
  ROS_INFO("Descending");
  return verticalMove(gripping_offset_ + bin_height);
}

bool PickNPlace::verticalMove(double target_z)
{
  ROS_INFO("Vertical move to target z: %f", target_z);

  while (ros::ok()) {
    // update the planning scene to get the robot's state
    moveit_msgs::PlanningScene planning_scene;
    planning_scene::PlanningScenePtr full_planning_scene;
    getPlanningScene(planning_scene, full_planning_scene);

    group_->getCurrentState()->update(true);

    geometry_msgs::Pose pose1 = group_->getCurrentPose().pose;
    geometry_msgs::Pose pose2 = pose1;
    ROS_INFO("Calling cart path from pose pos = (%.2f, %.2f, %.2f), quat = (%.2f, %.2f, %.2f, w %.2f)",
        pose1.position.x,
        pose1.position.y,
        pose1.position.z,
        pose1.orientation.x,
        pose1.orientation.y,
        pose1.orientation.z,
        pose1.orientation.w);

    pose1.position.z = (pose1.position.z+target_z)/2.0;
    pose2.position.z = target_z;
    ROS_INFO("for pose pos = (%.2f, %.2f, %.2f), quat = (%.2f, %.2f, %.2f, w %.2f)",
        pose2.position.x,
        pose2.position.y,
        pose2.position.z,
        pose2.orientation.x,
        pose2.orientation.y,
        pose2.orientation.z,
        pose2.orientation.w);
    // find linear trajectory
    moveit_msgs::RobotTrajectory lin_traj_msg, lin_traj_test_msg;
    std::vector<geometry_msgs::Pose> waypoints;
    // waypoints.push_back(pose1);
    waypoints.push_back(pose2);

    moveit_msgs::GetCartesianPath::Request req;
    moveit_msgs::GetCartesianPath::Response res;
    req.group_name = "arm";
    req.header.frame_id = "base_link";
    req.header.stamp = ros::Time::now();
    req.waypoints = waypoints;
    req.max_step = 0.05;
    req.jump_threshold = 0.0;
    req.avoid_collisions = true;
    robot_state::robotStateToRobotStateMsg(*group_->getCurrentState(), req.start_state);
    if (!cartesian_path_service_.call(req, res))
      return false;
    if (res.error_code.val != 1) {
      ROS_ERROR("cartesian_path_service_ returned with error code %d", res.error_code.val);
      return false;
    }
    double fraction = res.fraction;
    lin_traj_msg = res.solution;

    // create new robot trajectory object
    robot_trajectory::RobotTrajectory lin_rob_traj(group_->getCurrentState()->getRobotModel(), "arm");

    // copy the trajectory message into the robot trajectory object
    lin_rob_traj.setRobotTrajectoryMsg(*group_->getCurrentState(), lin_traj_msg);

    trajectory_processing::IterativeParabolicTimeParameterization iter_parab_traj_proc;
    if(!iter_parab_traj_proc.computeTimeStamps(lin_rob_traj)) {
      ROS_ERROR("Failed smoothing trajectory");
      return false;
    }

    // put the smoothed trajectory back into the message....
    lin_rob_traj.getRobotTrajectoryMsg(lin_traj_msg);

    MoveGroupPlan lin_traj_plan;
    lin_traj_plan.trajectory_ = lin_traj_msg;
    ROS_INFO("computeCartesianPath fraction = %f", fraction);
    if(fraction < 0.0) {
      ROS_ERROR("Failed computeCartesianPath");
      return false;
    }

    if (executeJointTrajectory(lin_traj_plan)) {
      ROS_INFO("Vertical joint trajectory execution successful");
      return true;
    }
    else {
      ROS_WARN("Vertical joint trajectory execution failed, going to restart");
      continue;
    }
  }
}



moveit_msgs::CollisionObjectPtr PickNPlace::getBinCollisionObject(int bin_number)
{
  std::string bin_name = "bin#" + boost::lexical_cast<std::string>(bin_number); 

  // update the planning scene to get the robot's state
  moveit_msgs::PlanningScene planning_scene;
  planning_scene::PlanningScenePtr full_planning_scene;
  getPlanningScene(planning_scene, full_planning_scene);

  for(int i=0;i<planning_scene.world.collision_objects.size();i++){
    if(planning_scene.world.collision_objects[i].id == bin_name){ 
      return moveit_msgs::CollisionObjectPtr(new moveit_msgs::CollisionObject(planning_scene.world.collision_objects[i]));
    }
  }
  ROS_ERROR("Failed to attach the bin. Attaching an empty collision object");
  return moveit_msgs::CollisionObjectPtr();
}

void PickNPlace::getObjectAbovePose(moveit_msgs::CollisionObjectPtr bin_coll_obj, geometry_msgs::Pose& pose, 
    double& bin_height)
{
  pose = bin_coll_obj->mesh_poses[0];

  // fix height
  bin_height = bin_coll_obj->meshes[0].vertices[0].z;
  pose.position.z = gripping_offset_+bin_height+dz_offset_;

  // fix orientation
  tf::Quaternion co_quat(pose.orientation.x, pose.orientation.y, 
      pose.orientation.z, pose.orientation.w);
  tf::Matrix3x3 m(co_quat);
  double roll, pitch, yaw;
  m.getRPY(roll, pitch, yaw);
  tf::Quaternion quat = tf::createQuaternionFromRPY(M_PI/2-yaw,M_PI/2,M_PI);
  pose.orientation.x = quat.x();
  pose.orientation.y = quat.y();
  pose.orientation.z = quat.z();
  pose.orientation.w = quat.w();
}



bool PickNPlace::deliverObject(int bin_number, double x_target, double y_target, double angle_target, double bin_height)
{
  ROS_INFO("Delivering to target (%.3f, %.3f, %f)", x_target, y_target, angle_target);

  if(!ascent(bin_height)) {
    ROS_ERROR("Failed to ascend while grasping bin.");
    return false;
  }
  // update planning scene
  if(!carryObjectTo(x_target, y_target, angle_target, bin_height)) {
    ROS_ERROR("Failed to carry bin to target (%.3f, %.3f, %.3f)", 
        x_target, y_target, angle_target);
    return false;
  }
  // update planning scene
  if(!descent(bin_height+0.02)) {
    ROS_ERROR("Failed to descend after moving bin above target place.");
    return false;
  }
  return true;
}



bool PickNPlace::carryObjectTo(double x_target, double y_target, double angle_target, double bin_height)
{
  ROS_INFO("Carrying bin to target (%.3f, %.3f, %f)", x_target, y_target, angle_target);
  geometry_msgs::Pose target_pose;
  getCarryObjectPose(x_target, y_target, angle_target, bin_height, target_pose);
  return traverseMove(target_pose);
}

void PickNPlace::getCarryObjectPose(double x_target, double y_target, double angle_target, double bin_height,
    geometry_msgs::Pose& pose)
{
  tf::Quaternion quat_goal = tf::createQuaternionFromRPY(M_PI/2-angle_target*M_PI/180.0, M_PI/2, M_PI);
  pose.position.x = x_target;
  pose.position.y = y_target;
  pose.position.z = gripping_offset_ + bin_height + dz_offset_;
  pose.orientation.x = quat_goal.x();
  pose.orientation.y = quat_goal.y();
  pose.orientation.z = quat_goal.z();
  pose.orientation.w = quat_goal.w();
}
*/

// bool PickNPlace::executeGripperAction(bool is_close, bool wait_for_result)
// {
//   if(is_close)
//     ROS_INFO("Closing gripper");
//   else
//     ROS_INFO("Opening gripper");
//   if(use_gripper) {
//     // send a goal to the action
//     control_msgs::GripperCommandGoal goal;
//     goal.command.position = (is_close) ? 0.0 : 0.08;
//     goal.command.max_effort = 100;
//     gripper_ac.sendGoal(goal);
//     if(wait_for_result)
//       return gripper_ac.waitForResult(ros::Duration(30.0));
//     else
//       return true;
//   }
//   else {
//     ros::Duration(2.0).sleep();
//     return true;
//   }
// }