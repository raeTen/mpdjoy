/*------------------- mpdjoy.c -------------------------*/
/*  Version: 0.9.1 					*/
/*  Author: neTear 2013 2014	, 			*/
/*  pseudonym at t-online de				*/
/*  Controls famous MPD by your linux-driven		*/
/*  joystick/joypad					*/
/*  Either as simple mpd client with output on nTTY or	*/
/*  without any output as a DAEMON			*/
/*  one instance  one pad/stick				*/
/*							*/
/** License: GPL V2 					*/
/*  no commercial use, no warrenties at all	 	*/
/*  works as designed ;)				*/
/*  0.9.1						*/
/*  optimized cpu usage and 				*/
/*  less socket traffic 				*/
/*  0.9.2 will support playlists, and will get further	*/
/*  improvement to polling and cpu usage		*/
/*------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <asm/types.h>
#include <syslog.h>
#include <signal.h>
#include <getopt.h>
#include <linux/joystick.h>

#include <mpd/client.h>

#include <libconfig.h>
#include <termios.h>
#include <libgen.h> 
#include <stdarg.h>
#include <assert.h>
#include <pwd.h> /** TODO uid/gid by name */

#define MPD_TIMEOUT 2000
#define DEADZONE 1
#define BUTTONZONE 9
/**volume control*/
#define FIXED true 
#define DYNAMIC false

#define IDLE 300
/**milliseconds | timer to poll input device driver*/
#define POLL 40 
/** lower values will occupy cpu more often, "80" is a good value for nowadays SoCs */
/* TODO POLL timer dynamic, depending on systems power */
/* it's a tightrope walk between cpu usage and (joystik/pad) reaction time */

#define DEBUG false

struct js_event e;
struct JS_DATA_TYPE js;

char rhost[40];
char rdevice[40];
int rport;
char rpassword[40];
char configfile[255];
char firstPlaylist[40];
uint daemonuser;
uint daemongroup;
uid_t uid;

typedef struct {
    int number;
    char action[40];
} actions;

actions jsbuttons[99];
actions jsaxis[99];

/*******************************/
/* mpd state definition */
typedef struct mpdstate {
   uint id;
   uint queuever;
   uint queuelen;
   uint crossfade;
   uint playmode;
   uint duration;
   uint elapsed;
   uint totalTime;
   int volume;
   int songid;
   enum mpd_state state;
} mpdstate;

/* mpd client */
typedef struct mpdclient {
   mpdstate state;
   struct mpd_connection *connection;
   struct mpd_status *status;
   struct mpd_playlist *playlist;
   struct mpd_song *song;
} mpdclient;

mpdclient *mpd = NULL;

enum {
   MPD_REPEAT = 0x1,
   MPD_RANDOM = 0x2,
   MPD_SINGLE = 0x4,
   MPD_CONSUME = 0x8
};

/*******************************************/
uint 			axpos = 0;
uint 			axbuf = 0;
uint 			axisret = 999;
uint 			userbuttons = 0;
uint 			useraxis = 0;
int			directvolume = 0;
int 			anim_cnt=0;
int 			dropevents=0;
char 			statusline[300];
char 			obuffer[300]; /*one line formatted output*/
uint			polltime=POLL;
uint			pollcnt=0;
int			relative_position=0;
/**signal handling ,logging*/
unsigned int		verbosity = 0;
bool			daemonize = false;
bool			running = true;
bool			isconnected = false;
/** ***************************************/
static int char_handle( c ) 
{
  switch ( c ) {
    case 'q':
      running=false;
      break;
    default:
      return 0;
  }
  return 0;
}

void handle_signal( int i ) 
{
  running = false;
  signal( i,  &handle_signal );
}

void logging( int level, const char *fmt, va_list ap ) 
{
  char buf[8192];
  if ( level == LOG_DEBUG && verbosity < 1 ) return;
  vsnprintf( buf,  sizeof( buf ), fmt , ap );
  if ( daemonize ) syslog( level, buf, "%s" );
    else 
    {
    FILE *out = ( level == LOG_DEBUG || level == LOG_ERR ? stderr : stdout );
    fprintf( out, "[%6s] ",
    level == LOG_DEBUG ?    "debug" :
    ( level == LOG_ERR ?     "error" :
    ( level == LOG_WARNING ? "warn" :
    ( level == LOG_NOTICE ?  "notice" :
    ( level == LOG_INFO ?    "info" : "(!?)" ) ) ) ) );
    fprintf( out, "%s", buf );
    }
}

void dolog(int level, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  logging(level, fmt, ap);
  va_end(ap);
}
/** **************************************************************/
void setsignal( void ) 
{
  signal(SIGHUP,  &handle_signal);
  signal(SIGTERM, &handle_signal);
  signal(SIGKILL, &handle_signal);
  signal(SIGILL,  SIG_IGN);
  signal(SIGABRT, SIG_IGN);
  signal(SIGUSR1, SIG_IGN);
  signal(SIGUSR2, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGSTOP, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);
  signal(SIGTTIN, SIG_IGN);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGUSR1, SIG_IGN);
  signal(SIGUSR2, SIG_IGN);
}

static int readconf() {
  config_t cfg;
  config_setting_t *setting;
  const char * str;
  const char * function;
  char lbuffer[255];
  //there were changes in libconfig, so change this (and both lines downwards)
  //according to what libconfig.h treats within config_setting_lookup_int()
  long int number;
  ///int number;
  config_init( &cfg );
  if ( ! config_read_file( &cfg, configfile) )
  {
    fprintf( stderr, "%d - %s\n", config_error_line( &cfg ), config_error_text( &cfg ) );
    config_destroy( &cfg );
    return( EXIT_FAILURE );
  }
  /** Get the basics. */
  if( config_lookup_string( &cfg, "host", &str) )
    sprintf( rhost, "%s", str );
  else
    sprintf( rhost, "%s", "localhost" ); 
  if( config_lookup_string( &cfg, "port", &str) )
    rport=atoi(str);
  else
    rport = 6600;
  if( config_lookup_string( &cfg, "device", &str ) )
    sprintf( rdevice, "%s", str );
  else
    sprintf( rdevice, "%s", "/dev/input/js0" );
  if( config_lookup_string( &cfg, "password", &str) )
    sprintf( rpassword, "%s", str );
  else
    sprintf( rpassword, "%s", "none" );
  if( config_lookup_string( &cfg, "firstPlaylist", &str) )
    sprintf( firstPlaylist, "%s", str );
  else
    sprintf( firstPlaylist, "%s", "none" );
  if( config_lookup_string( &cfg, "daemonuser", &str) )
    daemonuser = atoi( str );
  else
    daemonuser = 0;
  if( config_lookup_string( &cfg, "daemongroup", &str) )
    daemongroup = atoi( str );
  else
    daemongroup = 0;
  if (DEBUG) { 
    printf("Host		: %s\n",rhost);
    printf("Port		: %d\n",rport);
    printf("Device	: %s\n",rdevice);
    printf("Passw 	: %s\n",rpassword);
    printf("1Playlist	: %s\n",firstPlaylist);
    printf("user	: %d\n",daemonuser);
    printf("group	: %d\n",daemongroup);
  }
  /** Buttons config */
  /* Output a list of all configured buttons ********************************************/
  setting = config_lookup(&cfg, "joystick.buttons");
  if( setting != NULL )
  {
    int count = config_setting_length( setting );
    int i;
    
    if ( DEBUG ) 
      printf( "%-30s  %-30s\n", "Function", "Button" );
    for( i = 0; i < count; ++i )
    {
      config_setting_t *cbutton = config_setting_get_elem( setting, i); 
      if ( !( config_setting_lookup_string( cbutton, "fc", &function )
           && config_setting_lookup_int( cbutton, "nr", &number) ) )
        continue;

      if ( DEBUG ) printf( "%-30s  %3ld\n", function, number ); //number is long int
      ///if ( DEBUG ) printf( "%-30s  %3d\n", function, number );     //number is int
      sprintf( lbuffer, "%s", function );
      strcpy( jsbuttons[i].action, lbuffer );
      jsbuttons[i].number = number;
    }
    userbuttons = count;
  }
  /** Axis config */
  setting = config_lookup( &cfg, "joystick.axis");
  if( setting != NULL )
  {
    int count = config_setting_length( setting );
    int i;
    if ( DEBUG ) printf( "%-30s  %-30s\n", "Function", "Axis" );
    for( i = 0; i < count; ++i )
    {
      config_setting_t *caxis = config_setting_get_elem( setting, i );
      if ( !( config_setting_lookup_string( caxis, "fc", &function )
                    && config_setting_lookup_int( caxis, "nr",  &number) ) )
        continue;
      if ( DEBUG ) printf( "%-30s  %3ld\n", function, number ); //number is long int
      ///if ( DEBUG ) printf( "%-30s  %3d\n", function, number );     //number is int
      sprintf( lbuffer, "%s", function );
      strcpy( jsaxis[i]. action, lbuffer );
      jsaxis[i].number = number;
    }
    useraxis = count;
  }
  config_destroy( &cfg );
  return ( EXIT_SUCCESS );
}
static int animate(int status) {
  anim_cnt++;
  if ( anim_cnt > 3 ) anim_cnt = 0;
  switch ( status )
  {
    case 1: /** stopped */
      //return 95;break;
      switch ( anim_cnt )
      {
	case 0: return 46;
	case 1: return 58;
	case 2: return 124;
	case 3: return 58;
	case 4: return 46;
      }
    case 2: /** playing */
      switch ( anim_cnt )
      {
	case 0: return 124;
	case 1: return 47;
	case 2: return 45;
	case 3: return 92;
	case 4: return 45;
      }
    case 3: /** paused */
      return 124;
    case 4:
      switch ( anim_cnt ) 
      {
	case 0: return 124;
	case 1: return 47;
	case 2: return 45;
	case 3: return 92;
	case 4: return 45;
      }
  }
  return 0;
}
/**  ----------------------   MPD CLIENT functions --------------------------------------*/
static void fail( const char *errstr, ... )
{
  va_list ap;
  va_start( ap,  errstr ); 
  vfprintf( stderr, errstr, ap );
  va_end( ap );
  exit( EXIT_FAILURE );
}

static void mpd_quit( void ) 
{
  assert( mpd );
  if ( mpd->connection ) 
    mpd_connection_free( mpd->connection );
  if ( mpd->status ) 
    mpd_status_free( mpd->status );
  if ( mpd->song ) 
    mpd_song_free( mpd->song );
  if ( mpd->playlist ) 
    mpd_playlist_free(mpd->playlist );
  free( mpd ); mpd = NULL;
}

static int mpd_init( void ) 
{
  if ( ! mpd ) 
  {
    if ( !(mpd = malloc( sizeof( mpdclient ) ) ) )
      fail( "\nmpdclient allocation failed\n" );
    memset( mpd, 0, sizeof( mpdclient ) );
  } /**mpd init*/
  if ( ! mpd->connection )
  {
    if ( !(mpd->connection = mpd_connection_new( rhost, rport, MPD_TIMEOUT ) ) ||
           mpd_connection_get_error( mpd->connection ) )
      return 0;
    isconnected = true;
  } /**intial connection*/
  return 1;
}

void mpd_seek( int seekto )
{
  //NOTE if seeking fails (with other clients, too), there might be a bug in mpd itself
  //or in conjunction with snd-LKMs. I 've issues with that on a cubie (sunxi...)
  int setto = mpd->state.totalTime * seekto / 100;
  dropevents = 2;
  mpd_run_seek_id( mpd->connection, mpd->state.songid, setto );
}

static int mpd_status(bool eventdriven)
{
  if ( ( daemonize ) && ( ! eventdriven ) )
    return true;
  if ( mpd->status ) 
    mpd_status_free( mpd->status );

  if ( ( mpd->status = mpd_run_status( mpd->connection ) ) )
  {
      mpd->state.id = mpd_status_get_update_id( mpd->status );
      mpd->state.songid = mpd_status_get_song_id( mpd->status );
      mpd->state.volume = mpd_status_get_volume( mpd->status );
      mpd->state.crossfade = mpd_status_get_crossfade( mpd->status );
      mpd->state.queuever = mpd_status_get_queue_version( mpd->status );
      mpd->state.queuelen = mpd_status_get_queue_length( mpd->status );
      mpd->state.elapsed = mpd_status_get_elapsed_time( mpd->status );
      mpd->state.totalTime=mpd_status_get_total_time( mpd->status );
      mpd->state.duration = mpd_status_get_total_time( mpd->status );
      mpd->state.state = mpd_status_get_state( mpd->status );

      mpd->song = mpd_run_current_song( mpd->connection );
      return mpd->state.songid;
  } 
  else {
    mpd_quit();
    isconnected = mpd_init();
    return 0;
  }
}

void set_mpd_relative_position(void) 
{
    if ( mpd->state.totalTime >0 )
      relative_position=( mpd->state.elapsed * 100) / mpd->state.totalTime;
    else 
      relative_position = 0;
}

/** TODO******************************/
void toggle_repeat( void ) 
{
   if ( mpd_status_get_repeat( mpd->status ) ) 
   {
      mpd->state.playmode |= MPD_REPEAT;
   } 
}
void toggle_single( void ) 
{
   if ( mpd_status_get_single( mpd->status ) )
      mpd->state.playmode |= MPD_SINGLE;
}
void toggle_random( void ) 
{
   if ( mpd_status_get_random( mpd->status ) )
      mpd->state.playmode |= MPD_RANDOM;
}

void toggle_consume( void ) 
{
  if ( mpd_status_get_consume( mpd->status) )
      mpd->state.playmode |= MPD_CONSUME;
}

void loadfirstplaylist() {
  if ( mpd->state.songid == -1 )
  {
    /**Todo clearing playlist fails, don't know why yet */
    if ( !mpd_command_list_begin( mpd->connection, false ) )
    fail( "\nCommand list begin failed\n" );
    mpd_send_playlist_clear( mpd->connection, "" );
    mpd_command_list_end( mpd->connection );
    if ( !mpd_response_finish( mpd->connection ) )
      fail( "\nFailed finishing command\n" );

    if ( !mpd_command_list_begin( mpd->connection, false ) )
      fail( "\nCommand list begin failed\n" );
    printf( "loading: %s\n", firstPlaylist );
    mpd_send_load( mpd->connection, firstPlaylist );
    mpd_command_list_end( mpd->connection );
    if ( !mpd_response_finish( mpd->connection ) )
      fail( "\nFailed finishing command\n" );
    mpd_send_play( mpd->connection );
  }
}

static int print_mpd_selected(int ws_col_size) 
{
   int cx = 0;
   uint animchar = 0;
   char *fallbc = NULL, *fallbd = NULL; 
   if ( ! mpd->song ) return 0;
   assert( mpd->song );

   const char *album = mpd_song_get_tag( mpd->song, MPD_TAG_ALBUM, 0 );
   const char *title = mpd_song_get_tag( mpd->song, MPD_TAG_TITLE, 0 );
   const char *artist = mpd_song_get_tag( mpd->song, MPD_TAG_ARTIST, 0 );
   if ( !title ) title = mpd_song_get_tag( mpd->song, MPD_TAG_NAME, 0 );
   if ( !artist ) artist = mpd_song_get_tag( mpd->song, MPD_TAG_ALBUM_ARTIST, 0 );
   if ( !artist ) artist = mpd_song_get_tag( mpd->song, MPD_TAG_COMPOSER, 0 );
   if ( !artist ) artist = mpd_song_get_tag( mpd->song, MPD_TAG_PERFORMER, 0 );
   if ( !album && (fallbd = strdup( mpd_song_get_uri(mpd->song)))) album = basename( dirname( fallbd ) );
   if ( !title && (fallbc = strdup( mpd_song_get_uri(mpd->song)))) title = basename( fallbc );

   if ( !artist ) artist = "n/a";
   if ( !album ) album = "n/a";
   if ( !title ) title = "n/a";

   int em, es, tm, ts;
   em = mpd->state.elapsed / 60;
   es = mpd->state.elapsed - em * 60;
   tm = mpd->state.totalTime / 60;
   ts = mpd->state.totalTime - tm * 60;
   if ( artist && album && title ) 
   {
     animchar=animate( mpd->state.state );
     cx = snprintf( statusline, ws_col_size, 
             "Vol:%d %c%c %d:%.2d/%d:%.2d (%d%c)|(%d-%d)Artist:%s  Album:%s  Title:%s -",
             mpd->state.volume,animchar,animchar,
             em, es,tm,ts,relative_position,37, mpd->state.songid,mpd->state.id,
             artist, album, title); 
   }
   if ( fallbd ) free( fallbd );
   if ( fallbc ) free( fallbc );
   return cx;
}

/* Pad/Stick Event handling  */

int get_pad_event( int fd ) 
{
  fcntl( fd, F_SETFL, O_NONBLOCK );
  read ( fd, &e, sizeof( struct js_event ) );

  if ( e.type == JS_EVENT_BUTTON) 
  {
    switch( e.value ) 
    {
      case 1:
	polltime = POLL;
	return e.number;
	break;
      default:
	e.type = 0;
	break;
    }
  }
  if ( e.type == JS_EVENT_AXIS ) 
  {
      axpos = (float)((32768 + e.value) / 65536.0) * 20;
      if ( axbuf != axpos ) 
      {
	polltime = POLL;
	axbuf = axpos;
	axisret = ( (e.number+1 ) *100 ) + axpos;
	return axisret;
      }
    e.type = 0;
  }
  return -1;
}

static int simplehash( char *val )
{
  int cnt = 0;
  int rv = 0;

  for ( cnt=0; cnt < strlen ( val ); cnt++ ) 
  {
      rv = rv + val[cnt];
  }
  return rv + cnt;
}

static int setvol( int setval, bool fixed ) 
{
  int vol;
  mpd_status( daemonize );
  vol = mpd->state.volume;
  if ( !fixed )
  {
    if ( setval < 5 ) setval = setval / 2;
    if ( setval > 9 ) setval = 12;
  }
  vol = vol + setval;
  if ( vol>=99 ) vol=99; if (vol<=0 ) vol = 0;
  mpd_send_set_volume( mpd->connection, vol );
  directvolume = vol;
  return 1;
}
static int setdirectvol( int setval ) 
{
  int vol;
  vol = setval + 9; vol = vol * 6;
  if ( vol >= 99 ) vol=99; if (vol <= 0 ) vol=0;
  mpd_send_set_volume( mpd->connection, vol );
  return 1;
}

static void mpd_functions( int funccode, int aux) 
{
  int db = 0;
  if ( db)
    printf( "\ncode=%d aux=%d\n", 
           funccode, aux ); /* throws hashes for (other new) functions */
    if ( aux == BUTTONZONE || aux == -BUTTONZONE )
    {
       mpd_status( daemonize ); /**event driven status*/
       if ( daemonize ) set_mpd_relative_position(); /** event driven while daemonized */
        switch( funccode ) //hashed functions
        {
          case 442: /** play */
            mpd_send_play( mpd->connection );
            break;
          case 458: /** stop */
            polltime = IDLE;
            mpd_send_stop( mpd->connection );
            break;
          case 547: /** pause */
            polltime = IDLE;
            mpd_send_pause( mpd->connection, 1 );
            break;
          case 1195: /** togglepause */
            mpd_send_toggle_pause( mpd->connection );
            break;
          case 1434: /** toggle_single */
            toggle_single();
            break;
          case 1391: /** toggle_repeat */
            toggle_repeat();
            break;
          case 451: /** next */
            mpd_send_next( mpd->connection );
            break;
          case 901: /** previous */
            mpd_send_previous( mpd->connection );
            break;
          /** volume_plusX (1-9) */
          case 1272: case 1273: case 1274: case 1275: case 1276: case 1277: case 1278: case 1279: case 1280:
            setvol( -(1271-funccode), FIXED );
            break;
          /** volume_minusX (1-9) */
          case 1377: case 1378: case 1379: case 1380: case 1381: case 1382: case 1383: case 1384: case 1385:
            setvol( (1376-funccode), FIXED);
            break;
          case 472: /** seek+ */
            if ( dropevents ) break;
            relative_position = relative_position + 3;
            mpd_seek(relative_position);
            break;
          case 474: /** seek- */
            if ( dropevents ) break;
            relative_position = relative_position - 3;
            mpd_seek( relative_position );
            break;
          /** seek_plusX */
          case 1030: case 1031: case 1032: case 1033: case 1034: case 1035: case 1036: case 1037: case 1038:
            if ( dropevents ) break;
            relative_position = relative_position + (-(1029-funccode));
            mpd_seek( relative_position );
            break;
          /** seek_minusX */
          case 1135: case 1136: case 1137: case 1138: case 1139: case 1140: case 1141: case 1142: case 1143:
            if ( dropevents ) break;
            relative_position = relative_position + (1134-funccode);
            mpd_seek( relative_position );
            break;
        }
      }
      if ( aux >= DEADZONE || aux <= -DEADZONE ) 
      {
        mpd_status( daemonize ); /**event driven status*/
        if ( daemonize ) set_mpd_relative_position(); /** event driven while daemonized */
        switch ( funccode ) {
          case 714: /** volume+ */
            break;
          case 716: /** volume-*/
            break;
          case 760: /** volume+-*/
            if ( aux > 1 )  setvol( aux, DYNAMIC ); else if ( aux < -1 ) setvol( aux, DYNAMIC );
            break;
          case 794: /** !volume+-*/
            aux = aux * -1;
            if ( aux > 1 )  setvol( aux, DYNAMIC); else if ( aux < -1 ) setvol( aux, DYNAMIC );
            break;
          case 1311: /** volumedirect */
            break;
          case 1345: /** !volumedirect */
            break;
          case 1352: /** nextprevious */
            if ( aux > 8) mpd_send_next( mpd->connection ); else if ( aux < -8) mpd_send_previous( mpd->connection );
            break;
          case 1386: /** !nextprevious*/
            aux = aux * -1;
            if ( aux > 8 ) mpd_send_next( mpd->connection ); else if ( aux < -8 ) mpd_send_previous( mpd->connection );
            break;
          case 1309: /** nextPlayList */
            break;
          case 1759: /** previousPlaylist */
            break;
          case 2210: /** nextpreviousPlaylist*/
            break;
          case 518: /** seek+- */
            relative_position = relative_position + aux;
            mpd_seek( relative_position );
            break;
          case 552: /** !seek+- */
            break;
        }
      }
      /** no DEADZONE */
      switch ( funccode ) 
      {
        case 1311: /** volumedirect */
          setdirectvol( aux );
          break;
       case 1345: /** !volumedirect */
          setdirectvol ( aux * -1 );
          break;
      }
}

int button_action( int button )
{
  int cnt;
  for ( cnt=0; cnt < userbuttons; cnt++ ) 
  {
    if ( jsbuttons[cnt].number == button ) 
    {
      mpd_functions( simplehash( jsbuttons[cnt].action), 9 );
      break; /* rof*/
    }
  }
  return 0;
}

int axis_action( int axis, int axisval )
{
  int cnt;
  for ( cnt=0; cnt < useraxis; cnt++ ) 
  {
    if ( jsaxis[cnt].number == axis) 
    {
      mpd_functions( simplehash( jsaxis[cnt].action ), axisval );
      break; /* rof*/
    }
  }
  return 0;
}

static int setpolltime( int jsevent, int lastaxisval ) 
{
  if ( !daemonize ) 
  {
    if ( mpd->state.state == 2 ) 
    {
      pollcnt = 1;
      return POLL;
    }
  }
  //if ( lastaxisval ) return POLL;
  if ( jsevent > -1 ) 
  {
    pollcnt = 0;
    return POLL;
  } 
  else 
  {
    if ( polltime == POLL )
      {
        pollcnt++;
      }
    if ( pollcnt < 24 && polltime == POLL) 
    {
       return POLL;
    }
    else 
    {
      pollcnt = 0;
      return IDLE;
    }
  }
  return IDLE;
}
/** config structs ------------------------------------------------*/
static struct option const long_options[] = 
{
	{"configfile",		required_argument,	NULL, 'c'},
	{"daemonize",		no_argument,		NULL, 'd'},
	{"foobar",		no_argument,		NULL, 'f'},
	{"help",		no_argument,		NULL, 'h'},
	{NULL,			no_argument,		NULL, 0},
};

static char short_options[] = "c:df:h";

/** -----------------------    MAIN  ------------------------------*/
int main( int argc, char **argv) 
{
  int fd;
  int jsevent;
  int lastbutton = 0;
  int lastaxis = 0;
  int lastaxisval = 0;
  int timediv = 0;

  int option_index, j, mfork;
  struct winsize ws;
  strcpy( configfile, "mpdjoy.cfg" );
  while ( (j = getopt_long(argc, argv, short_options, long_options, &option_index)) != EOF)
  {
    switch ( j ) 
    {
      case 'd':
        daemonize = true;
        break;
      case 'c':
        sprintf( configfile, "%s", optarg );
        break;
      case 'h':
        puts( "-c <configuration file>" );
        puts( "-d run as daemon" );
        puts( "-h this help" );
        exit( 0 );
    }
  }

  printf( "Reading config %s \n", configfile );

  if ( readconf() )
  { 
    printf( "Error with configuration file <%s>\n", configfile );
    exit(1);
  }
  setsignal();

  puts( "Trying to connect to mpd..." );
  if ( mpd_init() )
  {
    printf( "Connection to mpd %s:%d established\n", rhost, rport);
    } 
    else 
    {
    printf( "Connection to mpd %s:%d FAILED\n", rhost, rport);
    fail( "\nIntial connection failed. Aborting. Please check the config for given mpd-host\n" );
    }
  if ( !( fd = open( rdevice, O_RDONLY ) ) ) 
  {
    fprintf( stderr, "Cannot open given input device\n" );
    }
    else
    {
    printf( "Input device on: %s\n", rdevice );
    }
  /** forking */
  if ( daemonize ) 
  {
    mfork = fork();
    if ( mfork < 0 )
    {
      dolog( LOG_ERR, "Couldn't fork to background\n" );
      return 1;
    }
  if ( mfork )
  {
    printf( "PID %d forked to background\n", mfork );
    dolog( LOG_INFO, "mpdjoy started as daemon with PID %d uid/gid %d/%d\n",
           mfork, daemonuser, daemongroup);
    }

  if ( mfork != 0 ) return 0; /* Exit first fork */
  /* second fork with dropped su access */
  //TODO: getpwnam(), could not get an "invalid" userids so setting by int for now, (take a look in pwd.h and struct passwd)
  if ( daemonuser == 0 ) {
    dolog( LOG_WARNING, "mpdjoy as DAEMON with root access -> Exiting!\n" );
    fail( "\nDAEMON running with root access - nope! - exiting!\n" );
    exit( 0 );
  }
  setuid(daemonuser); setgid(daemongroup); setsid();
  freopen("/dev/null","r",stdin);
  freopen("/dev/null","w",stdout);
  freopen("/dev/null","w",stderr); 
  }
/** -----------------------------*/
  fd_set set;
  struct timeval timevalue;
  struct termios oSets, nSets;
  tcgetattr( fileno( stdin ), &oSets );
  nSets = oSets;
  nSets.c_lflag &= ( ~ICANON & ~ECHO);
  tcsetattr( fileno( stdin ), TCSANOW, &nSets );
  while ( running )
  {
    timevalue.tv_sec = 0;
    timevalue.tv_usec = polltime * 1000; 
    if ( !daemonize ) 
    {
      FD_ZERO( &set );
      FD_SET( fileno( stdin ), &set );
      int res = select( fileno( stdin )+1, &set, NULL, NULL, &timevalue );
      timediv++;
      if( res > 0 )
      {
        char c;
        read( fileno( stdin ), &c, 1 );
        char_handle( c );
      }
      /** another select for timing purposes, termninal input code goes here, 
       additional KEY_EVENT handling e.g., not used within daemon*/
      /*main reason 4 this: smoother axis handling and status animation*/
    } else 
    { 
        select(0, NULL, NULL, NULL, &timevalue);
        timediv++;
    }
    if ( timediv>1 ) 
    {
      timediv = 0;
      dropevents--; 
      if ( dropevents <= 0 )
        dropevents = 0;
    }
    /**connect & update status*/
    if ( mpd_status( false ) != -1 ) 
    {
      if ( isconnected ) {
       if ( !daemonize ) set_mpd_relative_position();
       /**output*/
        if ( !daemonize ) 
        {
          ioctl(0, TIOCGWINSZ, &ws);
          print_mpd_selected( ws.ws_col );
          if ( DEBUG )
              snprintf( obuffer, ws.ws_col,
                       "%s Last Button:%d Last Axis:%d(%d) - %d - %d",
                       statusline,lastbutton,lastaxis,lastaxisval,pollcnt,dropevents);
            else
           snprintf( obuffer, ws.ws_col, "%s Last Button:%d Last Axis:%d(%d)",
                     statusline, lastbutton, lastaxis, lastaxisval );
         /** TODO add whitespace to obuffer according to ws_col (terminal length size) */
         printf( "\r%s", obuffer );
         fflush( stdout );
       }
        /** pad handling*/
        if ( lastaxisval && !dropevents )
        {
          axis_action( lastaxis, lastaxisval);
          dropevents=2;
        }
        jsevent=get_pad_event(fd);
        polltime=setpolltime(jsevent,lastaxisval);
        if ( jsevent >  -1 ) 
        {
          if (jsevent < 99 ) 
          {
            lastbutton = jsevent; 
            button_action( jsevent );
          }
          if ( jsevent > 99 ) 
          {
            //Todo: ignore defined axis for better useability
            lastaxisval = (jsevent - jsevent/100*100 ) - 9.5; 
            lastaxis= ( jsevent/100 ) - 1;
          }
        }
      } /** <- init and connection ok */
      else 
      {
        polltime=IDLE * 10;
        if ( !daemonize ) 
        {
          uint animchar = animate( 4 );
          printf( "\rTrying to (re)connect %c                                                                                        ",
                 animchar );
          fflush( stdout );
        }
       //TODO Playlist
      }
    } /** <- mpd status -1 missing playlist*/
    else 
    {
      polltime = IDLE * 10;
      if ( !daemonize ) 
      {
        uint animchar = animate( 4 );
        printf( "\rUnknown mpd status, Try to load a playlist %c                                                                     ",
               animchar );
        fflush( stdout );
      }
    }

  } /** loop */

  dolog( LOG_INFO, "mpdjoy shutting down\n" );
  if ( !daemonize ) puts( "Closing" );
  free( mpd );
  return 0;
}
