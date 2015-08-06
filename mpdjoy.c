/*------------------- mpdjoy.c -------------------------*/
/*  Version: 0.9.2 					*/
/*  Author: neTear c() 2013 2014 2015 			*/
/*                     					*/
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
/*  0.9.2 supports playlists next/prev 			*/
/* 							*/
/*------------------------------------------------------*/
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
#define BUTTONZONE 10
/** volume control----------------*/
#define FIXED true 
#define DYNAMIC false

#define IDLE 300
/** milliseconds | timer to poll input device driver----*/
#define POLL 40 
/* lower values will occupy cpu more often, "80" is a good value for nowadays SoCs */

#define MAX_CFG_ENTRIES  99
#define MAX_PLAYLISTS_ENTRIES 255 /*entity list from server*/
#define MAX_CA_LEN 99
#define DEBUG false
struct js_event e;
struct JS_DATA_TYPE js;

char rhost[MAX_CA_LEN];
char rdevice[MAX_CA_LEN];
int rport;
char rpassword[MAX_CA_LEN];
char configfile[255];
char firstPlaylist[MAX_CA_LEN];
uint daemonuser;
uint daemongroup;
uid_t uid;

typedef struct {
    int number;
    int norepeat;
    char cfg_value[MAX_CA_LEN];
} config_vals;

config_vals jsbuttons[MAX_CFG_ENTRIES];
config_vals jsaxis[MAX_CFG_ENTRIES];
config_vals knownPlaylists[MAX_CFG_ENTRIES];

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
uint            userplaylists = 0;
uint            serverplaylists = 0;
int			    directvolume = 0;
int 			anim_cnt=0;
int 			dropevents=0;
char 			statusline[300];
char 			obuffer[300]; /*one line formatted output*/
uint			polltime = POLL;
uint			pollcnt = 0;
int			    relative_position = 0;
int             lastAux = 0;
int             actPlaylist = 0;
/**signal handling ,logging*/
uint    		verbosity = 0;
bool			daemonize = false;
bool			running = true;
bool			isconnected = false;
bool            forcePlaying = false;
bool            useServerlist = false;
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
/** */
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
  const char * c_value;
  char lbuffer[MAX_CA_LEN];
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
  /** host and misc values. */
  if( config_lookup_string( &cfg, "host", &str) )
    snprintf( rhost, MAX_CA_LEN, "%s", str );
  else
    sprintf( rhost, "%s", "localhost" ); 
  if( config_lookup_string( &cfg, "port", &str) )
    rport=atoi(str);
  else
    rport = 6600;
  if( config_lookup_string( &cfg, "device", &str ) )
    snprintf( rdevice, MAX_CA_LEN, "%s", str );
  else
    sprintf( rdevice, "%s", "/dev/input/js0" );
  if( config_lookup_string( &cfg, "password", &str) )
    snprintf( rpassword, MAX_CA_LEN, "%s", str );
  else
    sprintf( rpassword, "%s", "none" );
  if( config_lookup_string( &cfg, "firstPlaylist", &str) )
    snprintf( firstPlaylist, MAX_CA_LEN, "%s", str );
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
  
  /** Known PLaylists */
  /* Output a list of all configured playlists */
  setting = config_lookup(&cfg, "knownPlaylists");
  if( setting != NULL )
  {
    int count = config_setting_length( setting );
    userplaylists = false;
    while ( ( userplaylists < count ) && ( userplaylists < MAX_CFG_ENTRIES ) )
    {
      config_setting_t *cplaylists = config_setting_get_elem( setting, userplaylists);
      if ( !( config_setting_lookup_string( cplaylists, "name", &c_value ) ) )
        continue;
      snprintf( lbuffer, MAX_CA_LEN, "%s", c_value );
      strcpy( knownPlaylists[userplaylists].cfg_value, lbuffer );
      knownPlaylists[userplaylists].number = userplaylists;
      knownPlaylists[userplaylists].norepeat = false;
      userplaylists++; 
    }

  }
  /** Buttons config */
  /* Output a list of all configured buttons */
  setting = config_lookup(&cfg, "joystick.buttons");
  if( setting != NULL )
  {
    int count = config_setting_length( setting );
    int i = 0;
    if ( DEBUG ) 
      printf( "%-30s  %-30s\n-------------\n", "Function", "Button" );
    while ( ( i < count ) && ( i < MAX_CFG_ENTRIES ) )
    {
      config_setting_t *cbutton = config_setting_get_elem( setting, i); 
      if ( !( config_setting_lookup_string( cbutton, "fc", &c_value )
           && config_setting_lookup_int( cbutton, "nr", &number) ) )
        continue;
      /*NOTE int or long int */
      if ( DEBUG ) printf( "%-30s  %3ld\n", c_value, number ); //number is long int
      ///if ( DEBUG ) printf( "%-30s  %3d\n", c_value, number );     //number is int
      snprintf( lbuffer, MAX_CA_LEN, "%s", c_value );
      strcpy( jsbuttons[i].cfg_value, lbuffer );
      jsbuttons[i].number = number;
      jsbuttons[i].norepeat = false;
      i++;
    }
    userbuttons = count;
  }
  /** Axis config */
  setting = config_lookup( &cfg, "joystick.axis");
  if( setting != NULL )
  {
    int count = config_setting_length( setting );
    int i = 0;
    if ( DEBUG ) printf( "%-30s  %-30s\n-------------\n", "Function", "Axis" );
    while ( ( i < count ) && ( i < MAX_CFG_ENTRIES ) )
    {
      config_setting_t *caxis = config_setting_get_elem( setting, i );
      if ( !( config_setting_lookup_string( caxis, "fc", &c_value )
                    && config_setting_lookup_int( caxis, "nr",  &number) ) )
        continue;
      /*NOTE int or long int */
      if ( DEBUG ) printf( "%-30s  %3ld\n", c_value, number ); //number is long int
      ///if ( DEBUG ) printf( "%-30s  %3d\n", c_value, number );     //number is int
      snprintf( lbuffer, MAX_CA_LEN, "%s", c_value );
      strcpy( jsaxis[i].cfg_value, lbuffer );
      jsaxis[i].number = number;
      jsaxis[i].norepeat = false;
      i++;
    }
    useraxis = count;
  }
  config_destroy( &cfg );
  return ( EXIT_SUCCESS );
}

void showConfig ( void )
{
  printf("Host        : %s\n",rhost);
  printf("Port        : %d\n",rport);
  printf("Device      : %s\n",rdevice);
  printf("mpd Password: %s\n",rpassword);
  printf("1Playlist   : %s\n",firstPlaylist);
  printf("user        : %d\n",daemonuser);
  printf("group       : %d\n",daemongroup);
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
/**  MPD CLIENT functions ---*/
static void fail( const char *errstr, ... )
{
  va_list ap;
  va_start( ap,  errstr ); 
  vfprintf( stderr, errstr, ap );
  va_end( ap );
  //exit( EXIT_FAILURE );
  running = false; /*otherwise ntty settings would not be resetted */

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
  if ( !mpd )
  {
    if ( !(mpd = malloc( sizeof( mpdclient ) ) ) )
      fail( "\nmpdclient allocation failed\n" );
    memset( mpd, 0, sizeof( mpdclient ) );
  } /**mpd (re)init*/
  if ( ! mpd->connection )
  {
    if ( !(mpd->connection = mpd_connection_new( rhost, rport, MPD_TIMEOUT ) ) ||
           mpd_connection_get_error( mpd->connection ) )
      return 0;
    isconnected = true;
  }
  
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

static bool mpd_status(bool eventdriven)
{
  if ( ( daemonize ) && ( ! eventdriven ) )
    return true;
  //if ( mpd->status )
    //mpd_status_free( mpd->status );
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
      if ( mpd->state.songid == -1 ) return false; else return true;
    }
    else
    {
      mpd_quit();
      isconnected = mpd_init();
      return true;
    }
  return true;
}

void set_mpd_relative_position(void) 
{
    if ( mpd->state.totalTime > 0 )
      relative_position=( mpd->state.elapsed * 100) / mpd->state.totalTime;
    else 
      relative_position = 0;
}

/** TODO */
void set_playmode( void )
{
}

int get_host_playlists( void )
{
  if (! mpd_send_list_meta(mpd->connection, "" ) )
    fail( "\nGetting playlists failed\n" );
  struct mpd_entity *entity;
  int i = 0;
  char lbuffer[MAX_CA_LEN];
  while ( ( entity = mpd_recv_entity( mpd->connection ) ) != NULL )
  {
    const struct mpd_playlist *tplaylist;
    enum mpd_entity_type entity_type = mpd_entity_get_type( entity );
    if ( entity_type == MPD_ENTITY_TYPE_PLAYLIST )
    {
      tplaylist = mpd_entity_get_playlist( entity );

      snprintf( lbuffer, MAX_CA_LEN, "%s", mpd_playlist_get_path( tplaylist ) );
      strcpy( knownPlaylists[i].cfg_value, lbuffer );
      knownPlaylists[i].number = i;
      knownPlaylists[i].norepeat = false;
      userplaylists = i;
      i++;
    }
  }
  return true;
}

int checkPlaylists ( void )
{
  /*using config or playlists from host*/
  switch ( userplaylists )
  {
      case 0:
          /*use Playlists from mpd host*/
      case 1:
          useServerlist = get_host_playlists();
          break;
      default:
          /*using the knownPlaylists from config*/
          break;
  }
  snprintf( firstPlaylist, MAX_CA_LEN, "%s", knownPlaylists[0].cfg_value );
  return true;//not evaluated
}

int showPlaylists ( void )
{
  int i = 0;
  puts("\nKnown Playlists:");
  for ( i=0;i < userplaylists; i++)
  {
    printf ( "%s\n",knownPlaylists[i].cfg_value );
  }
  return true;
}

int loadPlaylist( int actstate )
{
    if ( ! mpd_command_list_begin( mpd->connection, false ) )
      fail( "\nFatal error with loading a playlist\n" );
    mpd_send_clear( mpd->connection );
    mpd_send_load( mpd->connection, knownPlaylists[actPlaylist].cfg_value );
    if ( actstate == 2 || forcePlaying )
      mpd_send_play( mpd->connection );
    //mpd_send_stop( mpd->connection );
    mpd_command_list_end( mpd->connection );
    if ( !mpd_response_finish( mpd->connection ) )
      dolog( LOG_ERR, "Failed setting Playlist %s - check your configuration, they are case-sensitive\n",
             knownPlaylists[actPlaylist].cfg_value );
      return false;
  return true;
}


void nextpreviousPlaylist( int step )
{
    if ( useServerlist) get_host_playlists();
    actPlaylist = actPlaylist + (step);
    if (actPlaylist < 0 ) actPlaylist = userplaylists;
    if (actPlaylist > userplaylists) actPlaylist = 0;

    loadPlaylist(mpd->state.state);
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
           funccode, aux ); /* throws hashes for (other new) funccode */
  if ( lastAux != funccode+aux )
  {
    lastAux=funccode+aux;

    if ( aux == BUTTONZONE || aux == -BUTTONZONE )
    {
       lastAux = false;
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
            /*set_playmode*/
            break;
          case 1391: /** toggle_repeat */
            /*set_playmode*/
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
      if ( ( aux >= DEADZONE || aux <= -DEADZONE ) && ( aux < BUTTONZONE || aux > -BUTTONZONE ) )
      {
        lastAux = false;
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
            nextpreviousPlaylist(1);
            break;
          case 1759: /** previousPlaylist */
            nextpreviousPlaylist(-1);
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
}

int button_action( int button )
{
  int cnt;
  for ( cnt=0; cnt < userbuttons; cnt++ ) 
  {
    if ( jsbuttons[cnt].number == button )
    {
      if  ( jsbuttons[cnt].norepeat <= (int)time(NULL) )
      {
        mpd_functions( simplehash( jsbuttons[cnt].cfg_value), BUTTONZONE );
      
        if ( ( strstr( jsbuttons[cnt].cfg_value, "next" ) ) || 
             ( strstr( jsbuttons[cnt].cfg_value, "prev" ) ) ||
             ( strstr( jsbuttons[cnt].cfg_value, "pause" ) ) )
          /*repeat with decay*/
          jsbuttons[cnt].norepeat = (int)time(NULL) + 1; /* 1 second */
          /* TODO trying to find a portable way to use ms|µs so "dropevents" becomes unneeded*/
      } else
      {
        break;
        return 0;
      }
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
      mpd_functions( simplehash( jsaxis[cnt].cfg_value ), axisval );
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
/** useless within daemon -*/
static int char_handle( c )
{
  switch ( c ) {
    case 'q':
      running = false;
      break;
    case 'p' :
      showPlaylists();
      break;
    case 'f' :
      printf("\nFirst Playlist is '%s'\n",firstPlaylist);
      break;
    case 's' :
      get_host_playlists();
    default:
      return 0;
  }
  return 0;
}
/** config structs --*/
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
  int jsevent = -1;
  int lastbutton = 0;
  int lastaxis = 0, actaxis = 0, lastaxisval = 0;
  int timediv = 0;
  int option_index, j, mfork;
  bool loadedPlaylist = false;
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
    checkPlaylists();
    if ( DEBUG ) showConfig();
    } 
    else 
    {
    printf( "Connection to mpd %s:%d FAILED\n", rhost, rport);
    fail( "\nInitial connection failed. Aborting. Please check the config for given mpd-host\n" );
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
  //TODO: getpwnam(), could not get an "invalid" userids so setting by int for now
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
  else
  {
    puts("q = quit p = show used playlists f = show first playlist s = load knownPlaylists from host");
  }
/** ---------------------------*/
  fd_set set;
  struct timeval timevalue;
  struct termios oSets, nSets;
  if (! daemonize )
  {
    tcgetattr( fileno( stdin ), &oSets );
    nSets = oSets;
    nSets.c_lflag &= ( ~ICANON & ~ECHO);
    tcsetattr( fileno( stdin ), TCSANOW, &nSets );
    ioctl(0, TIOCGWINSZ, &ws);
  }
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
      /** another select for timing purposes,*/
    }
    else /*daemonized*/
    { 
        select(0, NULL, NULL, NULL, &timevalue);
        timediv++;
    }
    if ( timediv > 1 )
    {
      timediv = 0;
      dropevents--; 
      if ( dropevents <= 0 )
        dropevents = 0;
    }

    /**(re)connected & updated status*/
    loadedPlaylist = ( mpd_status( false ));
    if ( isconnected )
    {
     if ( !daemonize && loadedPlaylist) set_mpd_relative_position();
     /**output*/
      if ( !daemonize && loadedPlaylist )
      {
        print_mpd_selected( ws.ws_col );
        if ( DEBUG )
        {
          char truncated[30];
          snprintf( truncated, 25 , "%s", statusline );
          snprintf( obuffer, ws.ws_col,
                     "%s Last Button:%d Last Axis:%d(%d) [%d|%d|%d|%d] %d                                   ",
                     truncated,lastbutton,lastaxis,lastaxisval,pollcnt,dropevents,timediv,jsevent,actPlaylist);
        } else
        {
          snprintf( obuffer, ws.ws_col, "%s Last Button:%d Last Axis:%d(%d)",
                   statusline, lastbutton, lastaxis, lastaxisval );
        /** TODO add whitespace to obuffer according to ws_col (terminal length size) */
        }
        printf( "\r%s", obuffer );
        fflush( stdout );
      }
      if ( !daemonize && !loadedPlaylist )
      {
        polltime = IDLE * 2;
        forcePlaying = true;
        uint animchar = animate( 4 );
        printf( "\rTrying to load a playlist %c                                                                     ",
               animchar );
        fflush( stdout );
      }
      else
      {
        forcePlaying = false;
      }
      /** pad handling*/
      ///if ( lastaxisval && !dropevents )
      if ( actaxis && !dropevents )
      {
        axis_action( actaxis, lastaxisval);
        actaxis = false;
        dropevents = 2;
      }
      jsevent = get_pad_event(fd);
      polltime = setpolltime(jsevent,lastaxisval);
      if ( jsevent >  -1 )
      {
        if (jsevent < 99 )
        {
          lastbutton = jsevent;
          button_action( jsevent );
        }
        if ( jsevent > 99 )
        {
          lastaxisval = (jsevent - jsevent/100*100 ) - 9.5;
          lastaxis = ( jsevent/100 ) - 1;
          actaxis = lastaxis;
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
    }
  } /** loop*/

  dolog( LOG_INFO, "mpdjoy shutting down\n" );
  if ( !daemonize )
  {
    tcsetattr( fileno( stdin ), TCSANOW, &oSets );
    puts( "Bye\n" );
  }
  free( mpd );
  return 0;
}
