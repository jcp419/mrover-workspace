#include "rover.hpp"
#include "utilities.hpp"
#include "rover_msgs/Joystick.hpp"

#include <cmath>
#include <iostream>

// Constructs a rover status object and initializes the navigation
// state to off.
Rover::RoverStatus::RoverStatus()
    : mCurrentState( NavState::Off )
{
    mAutonState.is_auton = false;
    // {-1, 0, 0} refers to the struct of an empty Target
    // which means distance = -1, bearing = 0, id = 0
    mCTargetLeft = {-1.0, 0, 0};
    mCTargetRight = {-1.0, 0, 0};
    mObstacle = {0, 0, -1.0}; // empty obstacle --> distance is  -1
    mTargetLeft = {-1.0, 0, 0};
    mTargetRight = {-1.0, 0, 0};
} // RoverStatus()

// Gets a reference to the rover's current navigation state.
NavState& Rover::RoverStatus::currentState()
{
    return mCurrentState;
} // currentState()

// Gets a reference to the rover's current auton state.
AutonState& Rover::RoverStatus::autonState()
{
    return mAutonState;
} // autonState()

// Gets a reference to the rover's course.
Course& Rover::RoverStatus::course()
{
    return mCourse;
} // course()

// Gets a reference to the rover's path.
deque<Waypoint>& Rover::RoverStatus::path()
{
    return mPath;
} // path()

// Gets a reference to the rover's current obstacle information.
Obstacle& Rover::RoverStatus::obstacle()
{
    return mObstacle;
} // obstacle()

// Gets a reference to the rover's current odometry information.
Odometry& Rover::RoverStatus::odometry()
{
    return mOdometry;
} // odometry()

// Gets a reference to the rover's first target's current information.
Target& Rover::RoverStatus::leftTarget()
{
    return mTargetLeft;
} // leftTarget()

Target& Rover::RoverStatus::rightTarget() 
{
    return mTargetRight;
} // rightTarget()

Target& Rover::RoverStatus::leftCacheTarget()
{
    return mCTargetLeft;
} // leftCacheTarget()

Target& Rover::RoverStatus::rightCacheTarget() 
{
    return mCTargetRight;
} // rightCacheTarget()

unsigned Rover::RoverStatus::getPathTargets()
{
  return mPathTargets;
} // getPathTargets()

int& Rover::RoverStatus::getLeftMisses()
{
    return countLeftMisses;
}

int& Rover::RoverStatus::getRightMisses()
{
    return countRightMisses;
}

int& Rover::RoverStatus::getLeftHits()
{
    return countLeftHits;
}

int& Rover::RoverStatus::getRightHits()
{
    return countRightHits;
}

// Assignment operator for the rover status object. Does a "deep" copy
// where necessary.
Rover::RoverStatus& Rover::RoverStatus::operator=( Rover::RoverStatus& newRoverStatus )
{
    mAutonState = newRoverStatus.autonState();
    mCourse = newRoverStatus.course();
    mPathTargets = 0;

    while( !mPath.empty() )
    {
        mPath.pop_front();
    }
    for( int courseIndex = 0; courseIndex < mCourse.num_waypoints; ++courseIndex )
    {
        auto &wp = mCourse.waypoints[ courseIndex ];
        mPath.push_back( wp );
        if ( wp.search || wp.gate ) {
            ++mPathTargets;
        }
    }
    mObstacle = newRoverStatus.obstacle();
    mOdometry = newRoverStatus.odometry();
    mTargetLeft = newRoverStatus.leftTarget();
    mTargetRight = newRoverStatus.rightTarget();
    mCTargetLeft = newRoverStatus.leftCacheTarget();
    mCTargetRight = newRoverStatus.rightCacheTarget();
    countLeftMisses = newRoverStatus.getLeftMisses();
    countRightMisses = newRoverStatus.getRightMisses();
    return *this;
} // operator=

// Constructs a rover object with the given configuration file and lcm
// object with which to use for communications.
Rover::Rover( const rapidjson::Document& config, lcm::LCM& lcmObject )
    : mRoverConfig( config )
    , mLcmObject( lcmObject )
    , mBearingPid( config[ "bearingPid" ][ "kP" ].GetDouble(),
                   config[ "bearingPid" ][ "kI" ].GetDouble(),
                   config[ "bearingPid" ][ "kD" ].GetDouble() )
    , mLongMeterInMinutes( -1 )
{
} // Rover()

// Sends a joystick command to drive forward from the current odometry
// to the destination odometry. This joystick command will also turn
// the rover small amounts as "course corrections".
// The return value indicates if the rover has arrived or if it is
// on-course or off-course.
DriveStatus Rover::drive( const Odometry& destination )
{
    double distance = estimateNoneuclid( mRoverStatus.odometry(), destination );
    double bearing = calcBearing( mRoverStatus.odometry(), destination );
    return drive( distance, bearing, false );
} // drive()

// Sends a joystick command to drive forward from the current odometry
// in the direction of bearing. The distance is used to determine how
// quickly to drive forward. This joystick command will also turn the
// rover small amounts as "course corrections". target indicates
// if the rover is driving to a target rather than a waypoint and
// determines which distance threshold to use.
// The return value indicates if the rover has arrived or if it is
// on-course or off-course.
DriveStatus Rover::drive( const double distance, const double bearing, const bool target )
{
    //if (target){
       //std::cout << roverStatus().leftCacheTarget().distance << std::endl;
   //}
    if( ( !target && distance < mRoverConfig[ "navThresholds" ][ "waypointDistance" ].GetDouble() ) ||
        ( target && distance < mRoverConfig[ "navThresholds" ][ "targetDistance" ].GetDouble() ) )
    {
        return DriveStatus::Arrived;
    }

    double destinationBearing = mod( bearing, 360 );
    throughZero( destinationBearing, mRoverStatus.odometry().bearing_deg ); // will go off course if inside if because through zero not calculated

    if( fabs( destinationBearing - mRoverStatus.odometry().bearing_deg ) < mRoverConfig[ "navThresholds" ][ "drivingBearing" ].GetDouble() )
    {
        double turningEffort = mBearingPid.update( mRoverStatus.odometry().bearing_deg, destinationBearing );
        //When we drive to a target, we want to go as fast as possible so one of the sides is fixed at one and the other is 1 - abs(turningEffort)
        //if we need to turn clockwise, turning effort will be postive, so left_vel will be 1, and right_vel will be in between 0 and 1
        //if we need to turng ccw, turning effort will be negative, so right_vel will be 1 and left_vel will be in between 0 and 1
        double left_vel = min(1.0, max(0.0, 1.0 + turningEffort));
        double right_vel = min(1.0,  max(0.0, 1.0 - turningEffort));
        publishAutonDriveCmd(left_vel, right_vel);
        return DriveStatus::OnCourse;
    }
    cerr << "offcourse\n";
    return DriveStatus::OffCourse;
} // drive()

// Sends a joystick command to drive in the given direction and to
// turn in the direction of bearing. Note that this version of drive
// does not calculate if you have arrive at a specific location and
// this must be handled outside of this function.
// The input bearing is an absolute bearing.
//TODO: I'm 90% sure this function is redundant, we should just remove it
void Rover::drive( const int direction, const double bearing )
{
    double destinationBearing = mod( bearing, 360 );
    throughZero( destinationBearing, mRoverStatus.odometry().bearing_deg );
    const double turningEffort = mBearingPid.update( mRoverStatus.odometry().bearing_deg, destinationBearing );
    //std::cout << "turning effort: " << turningEffort << std::endl;
    double left_vel = min(1.0, max(0.0, 1.0 + turningEffort));
    double right_vel = min(1.0,  max(0.0, 1.0 - turningEffort));
    std::cout << "publishing drive command: " << left_vel << " , " << right_vel << std::endl;
    publishAutonDriveCmd(left_vel, right_vel);
} // drive()

// Sends a joystick command to turn the rover toward the destination
// odometry. Returns true if the rover has finished turning, false
// otherwise.
bool Rover::turn( Odometry& destination )
{
    double bearing = calcBearing( mRoverStatus.odometry(), destination );
    return turn( bearing );
} // turn()

// Sends a joystick command to turn the rover. The bearing is the
// absolute bearing. Returns true if the rover has finished turning, false
// otherwise.
bool Rover::turn( double bearing )
{
    bearing = mod( bearing, 360 );
    throughZero( bearing, mRoverStatus.odometry().bearing_deg );
    double turningBearingThreshold;
    if( isTurningAroundObstacle( mRoverStatus.currentState() ) )
    {
        turningBearingThreshold = 0;
    }
    else
    {
        turningBearingThreshold = mRoverConfig[ "navThresholds" ][ "turningBearing" ].GetDouble();
    }
    if( fabs( bearing - mRoverStatus.odometry().bearing_deg ) <= turningBearingThreshold )
    {
        return true;
    }
    double turningEffort = mBearingPid.update( mRoverStatus.odometry().bearing_deg, bearing );
    std::cout << "cur bearing: " << mRoverStatus.odometry().bearing_deg << " target bearing: " << bearing << " effort: " << turningEffort << std::endl;
    double minTurningEffort = mRoverConfig[ "navThresholds" ][ "minTurningEffort" ].GetDouble() * ( turningEffort < 0 ? -1 : 1 );
    if( isTurningAroundObstacle( mRoverStatus.currentState() ) && fabs( turningEffort ) < minTurningEffort )
    {
        turningEffort = minTurningEffort;
    }
    //to turn in place we apply +turningEffort, -turningEffort on either side and make sure they're both within [-1, 1]
    double left_vel = max(min(1.0, turningEffort), -1.0);
    double right_vel = max(min(1.0, -turningEffort), -1.0);
    std::cout << left_vel << ", " << right_vel << std::endl;
    publishAutonDriveCmd(left_vel, right_vel);
    return false;
} // turn()

// Sends a joystick command to stop the rover.
void Rover::stop()
{
    	std::cout << "stopping" << std::endl;
	publishAutonDriveCmd(0.0, 0.0);
} // stop()

// Checks if the rover should be updated based on what information in
// the rover's status has changed. Returns true if the rover was
// updated, false otherwise.
// TODO: unconditionally update everygthing. When abstracting search class
// we got rid of NavStates TurnToTarget and DriveToTarget (oops) fix this soon :P
bool Rover::updateRover( RoverStatus newRoverStatus )
{
    // Rover currently on.
    if( mRoverStatus.autonState().is_auton )
    {
        // Rover turned off
        if( !newRoverStatus.autonState().is_auton )
        {
            mRoverStatus.autonState() = newRoverStatus.autonState();
            return true;
        }

        // If any data has changed, update all data
        if( !isEqual( mRoverStatus.obstacle(), newRoverStatus.obstacle() ) ||
            !isEqual( mRoverStatus.odometry(), newRoverStatus.odometry() ) ||
            !isEqual( mRoverStatus.leftTarget(), newRoverStatus.leftTarget()) ||
            !isEqual( mRoverStatus.rightTarget(), newRoverStatus.rightTarget()) )
        {
            mRoverStatus.obstacle() = newRoverStatus.obstacle();
	    std::cout << "updating odom" << std::endl;
	    mRoverStatus.odometry() = newRoverStatus.odometry();
            mRoverStatus.leftTarget() = newRoverStatus.leftTarget();
            mRoverStatus.rightTarget() = newRoverStatus.rightTarget();

            // Cache Left Target if we had detected one
            if( mRoverStatus.leftTarget().distance != mRoverConfig[ "navThresholds" ][ "noTargetDist" ].GetDouble() ) 
            {

                // Associate with single post
                if( mRoverStatus.leftTarget().id == mRoverStatus.path().front().id )
                {
                    mRoverStatus.getLeftHits()++;
                }
else
                {
                    mRoverStatus.getLeftHits() = 0;
                }

                // Update leftTarget if we have 3 or more consecutive hits
                if( mRoverStatus.getLeftHits() >= 3 )
                {
                    mRoverStatus.leftCacheTarget() = mRoverStatus.leftTarget();
                    mRoverStatus.getLeftMisses() = 0;
                }

                // Cache Right Target if we had detected one (only can see right if we see the left one, otherwise
                // results in some undefined behavior)
                if( mRoverStatus.rightTarget().distance != mRoverConfig[ "navThresholds" ][ "noTargetDist" ].GetDouble() ) 
                {
                    mRoverStatus.rightCacheTarget() = mRoverStatus.rightTarget();
                    mRoverStatus.getRightMisses() = 0;
                }
                else 
                {
                    mRoverStatus.getRightMisses()++;
                }
            }
            else 
            { 
                mRoverStatus.getLeftMisses()++;
                mRoverStatus.getRightMisses()++; // need to increment since we don't see both
                mRoverStatus.getLeftHits() = 0;
                mRoverStatus.getRightHits() = 0;
            }

            // Check if we need to reset left cache
            if( mRoverStatus.getLeftMisses() > mRoverConfig[ "navThresholds" ][ "cacheMissMax" ].GetDouble() )
            {
                mRoverStatus.getLeftMisses() = 0;
                mRoverStatus.getLeftHits() = 0;
                // Set to empty target
                mRoverStatus.leftCacheTarget() = {-1, 0, 0};
            }

            // Check if we need to reset right cache
            if( mRoverStatus.getRightMisses() > mRoverConfig[ "navThresholds" ][ "cacheMissMax" ].GetDouble() )
            {
                mRoverStatus.getRightMisses() = 0;
                mRoverStatus.getRightHits() = 0;
                // Set to empty target
                mRoverStatus.rightCacheTarget() = {-1, 0, 0};
            }
            
            return true;
        }
        return true;
    }
    // Rover currently off.
    else
    {
        // Rover turned on.
        if( newRoverStatus.autonState().is_auton )
        {
            mRoverStatus = newRoverStatus;
            // Calculate longitude minutes/meter conversion.
            mLongMeterInMinutes = 60 / ( EARTH_CIRCUM * cos( degreeToRadian(
                mRoverStatus.odometry().latitude_deg, mRoverStatus.odometry().latitude_min ) ) / 360 );
            return true;
        }
        return false;
    }
} // updateRover()

// Calculates the conversion from minutes to meters based on the
// rover's current latitude.
const double Rover::longMeterInMinutes() const
{
    return mLongMeterInMinutes;
}

// Gets the rover's status object.
Rover::RoverStatus& Rover::roverStatus()
{
    return mRoverStatus;
} // roverStatus()

// Gets the rover's turning pid object.
PidLoop& Rover::bearingPid()
{
    return mBearingPid;
} // bearingPid()

void Rover::publishAutonDriveCmd( const double leftVel, const double rightVel)
{
    AutonDriveControl driveControl;
    driveControl.left_percent_velocity = leftVel;
    driveControl.right_percent_velocity = rightVel;
    //std::cout << leftVel << " " << rightVel << std::endl;
    string autonDriveControlChannel = mRoverConfig[ "lcmChannels" ][ "autonDriveControlChannel" ].GetString();
    mLcmObject.publish( autonDriveControlChannel, &driveControl) ;
}

// Returns true if the two obstacle messages are equal, false
// otherwise.
bool Rover::isEqual( const Obstacle& obstacle1, const Obstacle& obstacle2 ) const
{
    if( obstacle1.distance == obstacle2.distance &&
        obstacle1.bearing == obstacle2.bearing )
    {
        return true;
    }
    return false;
} // isEqual( Obstacle )

// Returns true if the two odometry messages are equal, false
// otherwise.
bool Rover::isEqual( const Odometry& odometry1, const Odometry& odometry2 ) const
{
    if( odometry1.latitude_deg == odometry2.latitude_deg &&
        odometry1.latitude_min == odometry2.latitude_min &&
        odometry1.longitude_deg == odometry2.longitude_deg &&
        odometry1.longitude_min == odometry2.longitude_min &&
        odometry1.bearing_deg == odometry2.bearing_deg )
    {
        return true;
    }
    return false;
} // isEqual( Odometry )

// Returns true if the two target messages are equal, false
// otherwise.
bool Rover::isEqual( const Target& target, const Target& target2 ) const
{
    if( target.distance == target2.distance &&
        target.bearing == target2.bearing )
    {
        return true;
    }
    return false;
} // isEqual( Target )

// Return true if the current state is TurnAroundObs or SearchTurnAroundObs,
// false otherwise.
bool Rover::isTurningAroundObstacle( const NavState currentState ) const
{
    if( currentState == NavState::TurnAroundObs ||
        currentState == NavState::SearchTurnAroundObs )
    {
        return true;
    }
    return false;
} // isTurningAroundObstacle()

/*************************************************************************/
/* TODOS */
/*************************************************************************/
