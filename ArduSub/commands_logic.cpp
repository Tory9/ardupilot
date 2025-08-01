#include "Sub.h"

#include <AP_RTC/AP_RTC.h>

static enum AutoSurfaceState auto_surface_state = AUTO_SURFACE_STATE_GO_TO_LOCATION;

// start_command - this function will be called when the ap_mission lib wishes to start a new command
bool Sub::start_command(const AP_Mission::Mission_Command& cmd)
{
    const Location &target_loc = cmd.content.location;
    auto alt_frame = target_loc.get_alt_frame();

    if (alt_frame == Location::AltFrame::ABOVE_HOME) {
        if (target_loc.alt > 0) {
            gcs().send_text(MAV_SEVERITY_WARNING, "Alt above home must be negative");
            return false;
        }
    } else if (alt_frame == Location::AltFrame::ABOVE_TERRAIN) {
        if (target_loc.alt < 0) {
            gcs().send_text(MAV_SEVERITY_WARNING, "Alt above terrain must be positive");
            return false;
        }
    } else {
        gcs().send_text(MAV_SEVERITY_WARNING, "Bad alt frame");
        return false;
    }

    switch (cmd.id) {

        ///
        /// navigation commands
        ///
    case MAV_CMD_NAV_WAYPOINT:                  // 16  Navigate to Waypoint
        do_nav_wp(cmd);
        break;

    case MAV_CMD_NAV_LAND:              // 21 LAND to Waypoint
        do_surface(cmd);
        break;

    case MAV_CMD_NAV_RETURN_TO_LAUNCH:
        do_RTL();
        break;

    case MAV_CMD_NAV_LOITER_UNLIM:              // 17 Loiter indefinitely
        do_loiter_unlimited(cmd);
        break;

    case MAV_CMD_NAV_LOITER_TURNS:              //18 Loiter N Times
        do_circle(cmd);
        break;

    case MAV_CMD_NAV_LOITER_TIME:              // 19
        do_loiter_time(cmd);
        break;

#if NAV_GUIDED
    case MAV_CMD_NAV_GUIDED_ENABLE:             // 92  accept navigation commands from external nav computer
        do_nav_guided_enable(cmd);
        break;
#endif

    case MAV_CMD_NAV_DELAY:                    // 93 Delay the next navigation command
        do_nav_delay(cmd);
        break;

        //
        // conditional commands
        //
    case MAV_CMD_CONDITION_DELAY:             // 112
        do_wait_delay(cmd);
        break;

    case MAV_CMD_CONDITION_DISTANCE:             // 114
        do_within_distance(cmd);
        break;

    case MAV_CMD_CONDITION_YAW:             // 115
        do_yaw(cmd);
        break;

        ///
        /// do commands
        ///
    case MAV_CMD_DO_CHANGE_SPEED:             // 178
        do_change_speed(cmd);
        break;

    case MAV_CMD_DO_SET_HOME:             // 179
        do_set_home(cmd);
        break;

    case MAV_CMD_DO_SET_ROI_LOCATION:       // 195
    case MAV_CMD_DO_SET_ROI_NONE:           // 197
    case MAV_CMD_DO_SET_ROI:                // 201
        // point the vehicle and camera at a region of interest (ROI)
        // ROI_NONE can be handled by the regular ROI handler because lat, lon, alt are always zero
        do_roi(cmd);
        break;

    case MAV_CMD_DO_MOUNT_CONTROL:          // 205
        // point the camera to a specified angle
        do_mount_control(cmd);
        break;

#if NAV_GUIDED
    case MAV_CMD_DO_GUIDED_LIMITS:                      // 222  accept guided mode limits
        do_guided_limits(cmd);
        break;
#endif

    default:
        // unable to use the command, allow the vehicle to try the next command
        gcs().send_text(MAV_SEVERITY_WARNING, "Ignoring command %d", cmd.id);
        return false;
    }

    // always return success
    return true;
}

/********************************************************************************/
// Verify command Handlers
/********************************************************************************/

// check to see if current command goal has been achieved
// called by mission library in mission.update()
bool Sub::verify_command_callback(const AP_Mission::Mission_Command& cmd)
{
    if (control_mode == Mode::Number::AUTO) {
        bool cmd_complete = verify_command(cmd);

        // send message to GCS
        if (cmd_complete) {
            gcs().send_mission_item_reached_message(cmd.index);
        }

        return cmd_complete;
    }
    return false;
}


// check if current mission command has completed
bool Sub::verify_command(const AP_Mission::Mission_Command& cmd)
{
    switch (cmd.id) {
        //
        // navigation commands
        //
    case MAV_CMD_NAV_WAYPOINT:
        return verify_nav_wp(cmd);

    case MAV_CMD_NAV_LAND:
        return verify_surface(cmd);

    case MAV_CMD_NAV_RETURN_TO_LAUNCH:
        return verify_RTL();

    case MAV_CMD_NAV_LOITER_UNLIM:
        return verify_loiter_unlimited();

    case MAV_CMD_NAV_LOITER_TURNS:
        return verify_circle(cmd);

    case MAV_CMD_NAV_LOITER_TIME:
        return verify_loiter_time();

#if NAV_GUIDED
    case MAV_CMD_NAV_GUIDED_ENABLE:
        return verify_nav_guided_enable(cmd);
#endif

    case MAV_CMD_NAV_DELAY:
        return verify_nav_delay(cmd);

        ///
        /// conditional commands
        ///
    case MAV_CMD_CONDITION_DELAY:
        return verify_wait_delay();

    case MAV_CMD_CONDITION_DISTANCE:
        return verify_within_distance();

    case MAV_CMD_CONDITION_YAW:
        return verify_yaw();

        // do commands (always return true)
    case MAV_CMD_DO_CHANGE_SPEED:
    case MAV_CMD_DO_SET_HOME:
    case MAV_CMD_DO_SET_ROI_LOCATION:
    case MAV_CMD_DO_SET_ROI_NONE:
    case MAV_CMD_DO_SET_ROI:
    case MAV_CMD_DO_MOUNT_CONTROL:
    case MAV_CMD_DO_SET_CAM_TRIGG_DIST:
    case MAV_CMD_DO_GUIDED_LIMITS:
        return true;

    default:
        // error message
        gcs().send_text(MAV_SEVERITY_WARNING,"Skipping invalid cmd #%i",cmd.id);
        // return true if we do not recognize the command so that we move on to the next command
        return true;
    }
}

// exit_mission - function that is called once the mission completes
void Sub::exit_mission()
{
    // play a tone
    AP_Notify::events.mission_complete = 1;

    // Try to enter loiter, if that fails, go to depth hold
    if (!mode_auto.auto_loiter_start()) {
        set_mode(Mode::Number::ALT_HOLD, ModeReason::MISSION_END);
    }
}

/********************************************************************************/
//  Nav (Must) commands
/********************************************************************************/

// do_nav_wp - initiate move to next waypoint
void Sub::do_nav_wp(const AP_Mission::Mission_Command& cmd)
{
    Location target_loc(cmd.content.location);
    // use current lat, lon if zero
    if (target_loc.lat == 0 && target_loc.lng == 0) {
        target_loc.lat = current_loc.lat;
        target_loc.lng = current_loc.lng;
    }
    // use current altitude if not provided
    if (target_loc.alt == 0) {
        // set to current altitude but in command's alt frame
        int32_t curr_alt;
        if (current_loc.get_alt_cm(target_loc.get_alt_frame(),curr_alt)) {
            target_loc.set_alt_cm(curr_alt, target_loc.get_alt_frame());
        } else {
            // default to current altitude as alt-above-home
            target_loc.copy_alt_from(current_loc);
        }
    }

    // this will be used to remember the time in millis after we reach or pass the WP.
    loiter_time = 0;
    // this is the delay, stored in seconds
    loiter_time_max = cmd.p1;

    // Set wp navigation target
    mode_auto.auto_wp_start(target_loc);
}

// do_surface - initiate surface procedure
void Sub::do_surface(const AP_Mission::Mission_Command& cmd)
{
    Location target_location;

    // if location provided we fly to that location at current altitude
    if (cmd.content.location.lat != 0 || cmd.content.location.lng != 0) {
        // set state to go to location
        auto_surface_state = AUTO_SURFACE_STATE_GO_TO_LOCATION;

        // calculate and set desired location below surface target
        // convert to location class
        target_location = Location(cmd.content.location);

        // decide if we will use terrain following
        int32_t curr_terr_alt_cm, target_terr_alt_cm;
        if (current_loc.get_alt_cm(Location::AltFrame::ABOVE_TERRAIN, curr_terr_alt_cm) &&
                target_location.get_alt_cm(Location::AltFrame::ABOVE_TERRAIN, target_terr_alt_cm)) {
            // if using terrain, set target altitude to current altitude above terrain
            target_location.set_alt_cm(curr_terr_alt_cm, Location::AltFrame::ABOVE_TERRAIN);
        } else {
            // set target altitude to current altitude above home
            target_location.set_alt_cm(current_loc.alt, Location::AltFrame::ABOVE_HOME);
        }
    } else {
        // set surface state to ascend
        auto_surface_state = AUTO_SURFACE_STATE_ASCEND;

        // Set waypoint destination to current location at zero depth
        target_location = Location(current_loc.lat, current_loc.lng, 0, Location::AltFrame::ABOVE_HOME);
    }

    // Go to wp location
    mode_auto.auto_wp_start(target_location);
}

void Sub::do_RTL()
{
    mode_auto.auto_wp_start(ahrs.get_home());
}

// do_loiter_unlimited - start loitering with no end conditions
// note: caller should set yaw_mode
void Sub::do_loiter_unlimited(const AP_Mission::Mission_Command& cmd)
{
    // convert back to location
    Location target_loc(cmd.content.location);

    // use current location if not provided
    if (target_loc.lat == 0 && target_loc.lng == 0) {
        // To-Do: make this simpler
        Vector3f temp_pos;
        wp_nav.get_wp_stopping_point_NE_cm(temp_pos.xy());
        const Location temp_loc(temp_pos, Location::AltFrame::ABOVE_ORIGIN);
        target_loc.lat = temp_loc.lat;
        target_loc.lng = temp_loc.lng;
    }

    // In mavproxy misseditor: Abs = 0 = ALT_FRAME_ABSOLUTE
    //                         Rel = 1 = ALT_FRAME_ABOVE_HOME
    //                         AGL = 3 = ALT_FRAME_ABOVE_TERRAIN
    //    2 = ALT_FRAME_ABOVE_ORIGIN, not an option in mavproxy misseditor

    // use current altitude if not provided
    // To-Do: use z-axis stopping point instead of current alt
    if (target_loc.alt == 0) {
        // set to current altitude but in command's alt frame
        int32_t curr_alt;
        if (current_loc.get_alt_cm(target_loc.get_alt_frame(),curr_alt)) {
            target_loc.set_alt_cm(curr_alt, target_loc.get_alt_frame());
        } else {
            // default to current altitude as alt-above-home
            target_loc.copy_alt_from(current_loc);
        }
    }

    // start way point navigator and provide it the desired location
    mode_auto.auto_wp_start(target_loc);
}

// do_circle - initiate moving in a circle
void Sub::do_circle(const AP_Mission::Mission_Command& cmd)
{
    Location circle_center(cmd.content.location);

    // default lat/lon to current position if not provided
    // To-Do: use stopping point or position_controller's target instead of current location to avoid jerk?
    if (circle_center.lat == 0 && circle_center.lng == 0) {
        circle_center.lat = current_loc.lat;
        circle_center.lng = current_loc.lng;
    }

    // default target altitude to current altitude if not provided
    if (circle_center.alt_is_zero()) {
        int32_t curr_alt;
        if (current_loc.get_alt_cm(circle_center.get_alt_frame(),curr_alt)) {
            // circle altitude uses frame from command
            circle_center.set_alt_cm(curr_alt,circle_center.get_alt_frame());
        } else {
            // default to current altitude above origin
            circle_center.copy_alt_from(current_loc);
            LOGGER_WRITE_ERROR(LogErrorSubsystem::TERRAIN, LogErrorCode::MISSING_TERRAIN_DATA);
        }
    }

    // calculate radius
    uint16_t circle_radius_m = HIGHBYTE(cmd.p1); // circle radius held in high byte of p1
    if (cmd.type_specific_bits & (1U << 0)) {
        circle_radius_m *= 10;
    }


    // true if circle should be ccw
    const bool circle_direction_ccw = cmd.content.location.loiter_ccw;

    // move to edge of circle (verify_circle) will ensure we begin circling once we reach the edge
    mode_auto.auto_circle_movetoedge_start(circle_center, circle_radius_m, circle_direction_ccw);
}

// do_loiter_time - initiate loitering at a point for a given time period
// note: caller should set yaw_mode
void Sub::do_loiter_time(const AP_Mission::Mission_Command& cmd)
{
    // re-use loiter unlimited
    do_loiter_unlimited(cmd);

    // setup loiter timer
    loiter_time     = 0;
    loiter_time_max = cmd.p1;     // units are (seconds)
}

#if NAV_GUIDED
// do_nav_guided_enable - initiate accepting commands from external nav computer
void Sub::do_nav_guided_enable(const AP_Mission::Mission_Command& cmd)
{
    if (cmd.p1 > 0) {
        // initialise guided limits
        mode_auto.guided_limit_init_time_and_pos();

        // set navigation target
        mode_auto.auto_nav_guided_start();
    }
}
#endif  // NAV_GUIDED

// do_nav_delay - Delay the next navigation command
void Sub::do_nav_delay(const AP_Mission::Mission_Command& cmd)
{
    nav_delay_time_start_ms = AP_HAL::millis();

    if (cmd.content.nav_delay.seconds > 0) {
        // relative delay
        nav_delay_time_max_ms = cmd.content.nav_delay.seconds * 1000; // convert seconds to milliseconds
    } else {
        // absolute delay to utc time
#if AP_RTC_ENABLED
        nav_delay_time_max_ms = AP::rtc().get_time_utc(cmd.content.nav_delay.hour_utc, cmd.content.nav_delay.min_utc, cmd.content.nav_delay.sec_utc, 0);
#else
        nav_delay_time_max_ms = 0;
#endif
    }
    gcs().send_text(MAV_SEVERITY_INFO, "Delaying %u sec", (unsigned)(nav_delay_time_max_ms/1000));
}

#if NAV_GUIDED
// do_guided_limits - pass guided limits to guided controller
void Sub::do_guided_limits(const AP_Mission::Mission_Command& cmd)
{
    mode_guided.guided_limit_set(cmd.p1 * 1000, // convert seconds to ms
                     cmd.content.guided_limits.alt_min * 100.0f,    // convert meters to cm
                     cmd.content.guided_limits.alt_max * 100.0f,    // convert meters to cm
                     cmd.content.guided_limits.horiz_max * 100.0f); // convert meters to cm
}
#endif

/********************************************************************************/
//  Verify Nav (Must) commands
/********************************************************************************/

// verify_nav_wp - check if we have reached the next way point
bool Sub::verify_nav_wp(const AP_Mission::Mission_Command& cmd)
{
    // check if we have reached the waypoint
    if (!wp_nav.reached_wp_destination()) {
        return false;
    }

    // play a tone
    AP_Notify::events.waypoint_complete = 1;

    // start timer if necessary
    if (loiter_time == 0) {
        loiter_time = AP_HAL::millis();
    }

    // check if timer has run out
    if (((AP_HAL::millis() - loiter_time) / 1000) >= loiter_time_max) {
        gcs().send_text(MAV_SEVERITY_INFO, "Reached command #%i",cmd.index);
        return true;
    }

    return false;
}

// verify_surface - returns true if surface procedure has been completed
bool Sub::verify_surface(const AP_Mission::Mission_Command& cmd)
{
    bool retval = false;

    switch (auto_surface_state) {
        case AUTO_SURFACE_STATE_GO_TO_LOCATION:
            // check if we've reached the location
            if (wp_nav.reached_wp_destination()) {
                // Set target to current xy and zero depth
                // TODO get xy target from current wp destination, because current location may be acceptance-radius away from original destination
                Location target_location(cmd.content.location.lat, cmd.content.location.lng, 0, Location::AltFrame::ABOVE_HOME);

                mode_auto.auto_wp_start(target_location);

                // advance to next state
                auto_surface_state = AUTO_SURFACE_STATE_ASCEND;
            }
            break;

        case AUTO_SURFACE_STATE_ASCEND:
            if (wp_nav.reached_wp_destination()) {
                retval = true;
            }
            break;

        default:
            // this should never happen
            // TO-DO: log an error
            retval = true;
            break;
    }

    // true is returned if we've successfully surfaced
    return retval;
}

bool Sub::verify_RTL() {
    return wp_nav.reached_wp_destination();
}

bool Sub::verify_loiter_unlimited()
{
    return false;
}

// verify_loiter_time - check if we have loitered long enough
bool Sub::verify_loiter_time()
{
    // return immediately if we haven't reached our destination
    if (!wp_nav.reached_wp_destination()) {
        return false;
    }

    // start our loiter timer
    if (loiter_time == 0) {
        loiter_time = AP_HAL::millis();
    }

    // check if loiter timer has run out
    return (((AP_HAL::millis() - loiter_time) / 1000) >= loiter_time_max);
}

// verify_circle - check if we have circled the point enough
bool Sub::verify_circle(const AP_Mission::Mission_Command& cmd)
{
    // check if we've reached the edge
    if (auto_mode == Auto_CircleMoveToEdge) {
        if (wp_nav.reached_wp_destination()) {
            Vector3f circle_center;
            UNUSED_RESULT(cmd.content.location.get_vector_from_origin_NEU_cm(circle_center));

            // set target altitude if not provided
            if (is_zero(circle_center.z)) {
                circle_center.z = inertial_nav.get_position_z_up_cm();
            }

            // set lat/lon position if not provided
            if (cmd.content.location.lat == 0 && cmd.content.location.lng == 0) {
                circle_center.xy() = inertial_nav.get_position_xy_cm();
            }

            // start circling
            mode_auto.auto_circle_start();
        }
        return false;
    }
    const float turns = cmd.get_loiter_turns();

    // check if we have completed circling
    return fabsf(sub.circle_nav.get_angle_total_rad()/M_2PI) >= turns;
}

#if NAV_GUIDED
// verify_nav_guided - check if we have breached any limits
bool Sub::verify_nav_guided_enable(const AP_Mission::Mission_Command& cmd)
{
    // if disabling guided mode then immediately return true so we move to next command
    if (cmd.p1 == 0) {
        return true;
    }

    // check time and position limits
    return mode_auto.guided_limit_check();
}
#endif  // NAV_GUIDED

// verify_nav_delay - check if we have waited long enough
bool Sub::verify_nav_delay(const AP_Mission::Mission_Command& cmd)
{
    if (AP_HAL::millis() - nav_delay_time_start_ms > nav_delay_time_max_ms) {
        nav_delay_time_max_ms = 0;
        return true;
    }
    return false;
}

/********************************************************************************/
//  Condition (May) commands
/********************************************************************************/

void Sub::do_wait_delay(const AP_Mission::Mission_Command& cmd)
{
    condition_start = AP_HAL::millis();
    condition_value = cmd.content.delay.seconds * 1000;     // convert seconds to milliseconds
}

void Sub::do_within_distance(const AP_Mission::Mission_Command& cmd)
{
    condition_value  = cmd.content.distance.meters;
}

void Sub::do_yaw(const AP_Mission::Mission_Command& cmd)
{
    sub.mode_auto.set_auto_yaw_look_at_heading(
        cmd.content.yaw.angle_deg,
        cmd.content.yaw.turn_rate_dps,
        cmd.content.yaw.direction,
        cmd.content.yaw.relative_angle);
}


/********************************************************************************/
// Verify Condition (May) commands
/********************************************************************************/

bool Sub::verify_wait_delay()
{
    if (AP_HAL::millis() - condition_start > (uint32_t)MAX(condition_value, 0)) {
        condition_value = 0;
        return true;
    }
    return false;
}

bool Sub::verify_within_distance()
{
    if (wp_nav.get_wp_distance_to_destination_cm() < (uint32_t)MAX(condition_value,0)) {
        condition_value = 0;
        return true;
    }
    return false;
}

// verify_yaw - return true if we have reached the desired heading
bool Sub::verify_yaw()
{
    // set yaw mode if it has been changed (the waypoint controller often retakes control of yaw as it executes a new waypoint command)
    if (auto_yaw_mode != AUTO_YAW_LOOK_AT_HEADING) {
        sub.mode_auto.set_auto_yaw_mode(AUTO_YAW_LOOK_AT_HEADING);
    }

    // check if we are within 2 degrees of the target heading
    return (abs(wrap_180_cd(ahrs.yaw_sensor-yaw_look_at_heading)) <= 200);
}

/********************************************************************************/
//  Do (Now) commands
/********************************************************************************/

// do_guided - start guided mode
bool Sub::do_guided(const AP_Mission::Mission_Command& cmd)
{
    // only process guided waypoint if we are in guided mode
    if (control_mode != Mode::Number::GUIDED && !(control_mode == Mode::Number::AUTO && auto_mode == Auto_NavGuided)) {
        return false;
    }

    // switch to handle different commands
    switch (cmd.id) {

    case MAV_CMD_NAV_WAYPOINT: {
        // set wp_nav's destination
        return sub.mode_guided.guided_set_destination(cmd.content.location);
    }

    case MAV_CMD_CONDITION_YAW:
        do_yaw(cmd);
        return true;

    default:
        // reject unrecognised command
        return false;
    }

    return true;
}

void Sub::do_change_speed(const AP_Mission::Mission_Command& cmd)
{
    if (cmd.content.speed.target_ms > 0) {
        wp_nav.set_speed_NE_cms(cmd.content.speed.target_ms * 100.0f);
    }
}

void Sub::do_set_home(const AP_Mission::Mission_Command& cmd)
{
    if (cmd.p1 == 1 || !cmd.content.location.initialised()) {
        if (!set_home_to_current_location(false)) {
            // silently ignore this failure
        }
    } else {
        if (!set_home(cmd.content.location, false)) {
            // silently ignore this failure
        }
    }
}

// do_roi - starts actions required by MAV_CMD_NAV_ROI
//          this involves either moving the camera to point at the ROI (region of interest)
//          and possibly rotating the vehicle to point at the ROI if our mount type does not support a yaw feature
//  TO-DO: add support for other features of MAV_CMD_DO_SET_ROI including pointing at a given waypoint
void Sub::do_roi(const AP_Mission::Mission_Command& cmd)
{
    sub.mode_auto.set_auto_yaw_roi(cmd.content.location);
}

// point the camera to a specified angle
void Sub::do_mount_control(const AP_Mission::Mission_Command& cmd)
{
#if HAL_MOUNT_ENABLED
    camera_mount.set_angle_target(cmd.content.mount_control.roll, cmd.content.mount_control.pitch, cmd.content.mount_control.yaw, false);
#endif
}
