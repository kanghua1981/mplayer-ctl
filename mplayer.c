
/*
Mplayer 控制程序，实现如下功能:
1.播放列表管理
2.管理播放进度
3.播放广告
4.响应播放按键
5.上报媒体文件tag信息
6.用户管理
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/inotify.h>


#include "XML/ixml.h"
#include "XML/xmldoc.h"
#include "TCPServer/connectmng.h"
#include "mplayer.h"


#include "server.h"
#include "msg_buf.h"

#define CONTRL_PORT  "/tmp/ctrl"

#define SCAN_REAL_AUDIO
/*tts functions*/
int ttsinit(void);
int ttsdeinit(void);
int playtts(char *strtext);

//#define PLAY_ADS

extern struct adplugin_server *g_adserver;
extern 
int server_xmit_play_ctl_info_api(struct adplugin_server *server, play_ctl_t ctl_event);

#if 0
extern FILE *logfile;
#define log(format,str...)   \
{\
    fprintf(logfile,"{%s:%d} "format,__FILE__,__LINE__,##str); \
    fflush(logfile); \
}
#endif
#define MAX_LINE_IN_CACHE   1024
typedef struct AudioCache_S
{
    int startline;
    int endline;
    int validnum;
    char line[MAX_LINE_IN_CACHE][512];
}AudioCahche;
AudioCahche audiolist = {0};
#define MUSIC_SYSCONFIG_PATH    "/tmp/mnt/USB-disk-a1/.system"
#define MUSIC_STATUS_FILE       "/tmp/mnt/USB-disk-a1/.system/music.status"
#define MUSIC_CONFIG_FILE       "/tmp/mnt/USB-disk-a1/.system/music.conf"
#define MUSIC_AUDIOLIST_FILE    "/tmp/mnt/USB-disk-a1/.system/audiolist"
//#define log printf
static int SYNC_TIMEOUT_NUM = 0;
static int GetLine(char *buffer,int size,int timeout);

int UpdateConfig(void);
int SetMute(void);
void NextMediaSigHandler(int signumber);
void PrevMediaSigHsandler(int signumber);
void PrevDirSigHandler(int signumber);
void NextDirSigHandler(int signumber);
void PauseSigHandler(int signumber);
void SettingSigHandler(int signumber);
static int UpdateStatus();
static int GetMeidaType(char *path);
static int CheckValidAudioFile(char *filename);
static int AddRadioAlbumXml (const char *path);
static SingleMedia *GetAudioFileByIdx(DirNode *dirnode,int idx);
static int AddXmlAudioToDir(DirNode *dir);
static void SetDirValid(DirNode *dirnode);
static void SetDirInvalid(DirNode *dirnode);
static void clearAllDirTable(void);
static int Play(SingleMedia *audio,int seconds);
extern void* adserver_init(void *param);

extern void stopnowtts();
volatile int stopttsflags = 0;
volatile int sleepforbutton = 0;

int begintoplay = 0;
int gpipe;
int gmpipe;
int FdPipe[2];
pthread_t PosThread;
pthread_t AdThread;

SingleMedia lastplayaudio = {0};

PlayerInfo g_player_info;


AdAduio g_adaudio = {0};


char *Volume="100.0";

char audiotype[][6] =
{
    "mp3",
    "MP3",
    "aac",
    "alac",
    "ogg",
    "ape",
    "m4a",
    "wav"
};


static IXML_Document *to_idoc(struct xmldoc *x) {
	return (IXML_Document*) x;
}

static IXML_Element *to_ielem(struct xmlelement *x) {
	return (IXML_Element*) x;
}


static int ParseTime(char *time_string) 
{
    float ftime;
	sscanf((char*)time_string, "%f", &ftime);
	const int seconds = (int)ftime;
	return seconds;
}


static int WriteCMDToMPlayer(char *cmd)
{
    int ret = 0;
    char strcmd[512] = {0};
    sprintf(strcmd,"%s\n",cmd);
    //printf(strcmd);
    ret = write(gmpipe,strcmd,strlen(strcmd));
    if (ret < 0)
    {
        log("write gmpipe failed : %s\n",cmd);
        return -1;
    }
    
    return SUCCESS;
}

void SetPlayStatus(int status)
{
    g_player_info.playstatus = status;
    /*
    if (status == MPLAYER_STOP)
    {
        SetMute();
        WriteCMDToMPlayer("stop");
    }
    */
#ifdef PLAY_ADS
    if (status == MPLAYER_PLAYING)
    {
    
        server_xmit_play_ctl_info_api(g_adserver,PLAY);
    }
    else if (status == MPLAYER_PAUSE)
    {
        server_xmit_play_ctl_info_api(g_adserver,PAUSE);
    }
    else if (status == MPLAYER_STOP)
    {
        server_xmit_play_ctl_info_api(g_adserver,STOP);
    }
#endif    
    UpdateConfig();
}

int GetPlayStatus()
{
    return g_player_info.playstatus;
}


void SyncLedCtl(int on)
{
    static int ctlon = -1;
    if (ctlon == on)
    {
        return;
    }
    ctlon = on;
    
    if (ctlon)
    {
        system("/usr/sbin/awctl ledsync");
        usleep(11000);
        system("/usr/sbin/awctl ledsync");
    }
    else
    {
        system("/usr/sbin/awctl ledon");
        usleep(11000);
        system("/usr/sbin/awctl ledon");
    }
}

void beginSync(int sync)
{
    g_player_info.syncstatus = sync;
    //AddAllDirWatch();
    SYNC_TIMEOUT_NUM = 0; /*收到开始同步心跳后，timeout清0*/
    SyncLedCtl(1);
}

static int GetPosition(char *track_duration,char *track_pos)
{
    char buf[256] = {0};
    int ret = 0;
 
    if (GetPlayStatus() != MPLAYER_PLAYING){
        return -2;  
    }
    pthread_mutex_lock(&(g_player_info.mutex));
    WriteCMDToMPlayer("get_property time_pos");
    while(ret = GetLine(buf,256,1000))
    {
         //printf("output_mplayer_get_position1:%s",buf);
         /*属性不存在，则说明播放完成或者未启动*/
        if(strncmp(buf,"ANS_time_pos",strlen("ANS_time_pos")) == 0)
            break;
        else
        {
            continue;
        }
         
        bzero(buf,256);
     }
     pthread_mutex_unlock(&(g_player_info.mutex));
     if (ret == 0)
     {
        log("output_mplayer_get_position timeout \n");
        return -1;
     }
     sscanf(buf,"ANS_time_pos=%s",(char*)track_pos);
     SetCurMediaCurTime(ParseTime(track_pos));
#if 0
     //printf("%d\n",g_player_info.curaudioinfo.curtime);
     pthread_mutex_lock(&(g_player_info.mutex));
     WriteCMDToMPlayer("get_property length");
     //while (fgets(buf,256,gpipe))
     while (ret = GetLine(buf,256,1000))
     {
         //printf("output_mplayer_get_position2:%s",buf);
         /*属性不存在，则说明播放完成或者未启动*/
        if(strncmp(buf,"ANS_length",strlen("ANS_length")) == 0)
            break;
        else
        {
            continue;
        }
        bzero(buf,256);
     }
     pthread_mutex_unlock(&(g_player_info.mutex));
     if (ret == 0)
     {
         log("output_mplayer_get_position timeout \n");
         
         return -1;
     }

     sscanf(buf,"ANS_length=%s",(char*)track_duration);

     SetCurMediaTotalTime(ParseTime(track_duration));
 #endif    
     return SUCCESS;
}

inline void UpdateSync(int sync)
{
    FILE *fp = NULL;
    fp = fopen(CONTRL_PORT"/syncbeat","w+");
    if (NULL == fp)
     return -1;
    fprintf(fp,"%d\n",sync);
    fflush(fp);
    fclose(fp);

    return;
}


 /*check playstatus every secend*/
static void* UpdateTrackTimeThread(void *param)
{
    char track_duration[16] = {0};
    char track_pos[16] = {0};
    int ret;
    int pos;
    int retrytimes = 4;
    SingleMedia *media = NULL;
    int updatecount = 0;
    
    while(1)
    {
        updatecount++;
        usleep(1000000);  //1000ms
        if ((updatecount % 5 == 0) && g_player_info.syncstatus == AUDIO_SYNC_PROCESS)
        {
            
            SYNC_TIMEOUT_NUM++; /*5s增加一次*/
            if (SYNC_TIMEOUT_NUM >= 12)
            {
                log("sync time heartbeat lost 60s, change to sync end\n");
                /*心跳丢失超时，自动设置同步完成*/
                g_player_info.syncstatus = AUDIO_SYNC_END;
                /*sync 心跳丢失，更新心跳文件，避免下次误判断*/
                UpdateSync(0);
                SYNC_TIMEOUT_NUM = 0;
                
#ifdef PLAY_ADS
                server_xmit_sync_info_api(g_adserver, APP_SYNC_END);
#endif
            }
           
#ifdef PLAY_ADS
            server_xmit_sync_info_api(g_adserver, APP_SYNC_START);	
#endif
        }
        media = g_player_info.curaudioinfo.media;
        if (media == NULL)
        {
            continue;
        }
retry: 
        ret = GetPosition(track_duration,track_pos);
        if (ret == -1)
        {
            retrytimes--;
            if (retrytimes == 0)
            {
                //memset(&(g_player_info.curaudioinfo),0,sizeof(CurAudioInfo));
                /*广告播放完毕后，使能按键*/
                if (media->mediatype == MEDIA_AD)
                {
                    g_player_info.button.disabled = 0;
                }
                
                UpdateCurMediaInfo(NULL,NULL);
                SetPlayStatus(MPLAYER_STOP);
                retrytimes = 4;
            }
            else
            {
                usleep(100000);
                goto retry;
            }
        }
        else if (ret == -2)
        {
            continue;
        }

        if (media == NULL)
        {
            continue;
        }
        //log("%s %s %d\n",media->dirname,media->medianame,media->mediatype);
        /*只有在AUDIO模式才需要记录当前播放时间*/
       // if ((updatecount % 5 == 0) && (media->mediatype == MEDIA_AUDIO))
       /*为了继续播放，必须记录所有情况,除广告外*/
        if (updatecount % 5 == 0 && media->mediatype != MEDIA_AD)
        {
            /*保存当前的进度，以及播放的歌曲*/
            //UpdataConfig();
            //log("kkkkkkkkkkkkk\n");
            UpdateStatus();
        }
    }
    
    return NULL;
}


static int GetLine(char *buffer,int size,int timeout)
{
    char bufchar;
    char *localbuffer = buffer;
    int ret = 0;
    int count = 0;
    int timenum = 0;
    while ((count < size) && (timenum < timeout))
    {
        ret = read(gpipe,&bufchar,1);
        if (ret < 0)
        {
            if (errno == EAGAIN)
            {
                //perror(""); 
                usleep(1000);
                timenum++;
                continue;
            }
            log("read failed\n");
        }
        else if (ret == 1)
        {
            if (bufchar == '\0')
            {
                *localbuffer++ = bufchar;
                break;
            }
            
            count++;
            *localbuffer++ = bufchar;
            if (bufchar == '\n')
            {
                //localbuffer++;
                *localbuffer = '\0';
                break;
            }
        }
    }
    if (timenum == timeout)
    {
        /*保持和fgets统一的返回值*/
        log("read timeout\n");
        return 0;
    }
    
    //printf("%s",buffer);
    return count;
}

static SingleMedia *GetDirFirstAudio(DirNode * dir)
{
    int audioidx = dir->audiolist[0];
    //log("first audio %d dir->audionum=%d\n",audioidx,dir->audionum);
    /*没有歌曲*/
    if (dir->audionum == 0)
        return NULL;
    return GetAudioFileByIdx(dir,audioidx);
}

static int GetDirAudioIdxByName(DirNode * dir,char *audioname)
{
    int i;
    int audioidx;
    SingleMedia *audio;
    if (NULL == dir || NULL == audioname)
        return -1;
    //log("GetDirAudioIdxByName:%d\n",dir->audionum);
    for (i = 0;i < dir->audionum;i++)
    {
        audioidx = dir->audiolist[i];
        //log("GetDirAudioIdxByName:i=%d audioid=%d\n",i,audioidx);
        audio = GetAudioFileByIdx(dir,audioidx);
        if(audio == NULL)
        {
            return -1;
        }
        //log("GetDirAudioIdxByName:audio=%p\n",audio);
        if (0 == strcmp(audio->medianame,audioname))
        {
            return i;
        }
    }
    return -1;
}

static int GetDirNum(void)
{
    return g_player_info.dirarray.listidx + 1;
}

static int GetDirIdxByName(char *pathname)
{
    int i;
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;
    DirNode *dir = NULL;
    
    for (i = 0;i < idx;i++)
    {
        dir = (DirNode*)(dirarray->list + i * sizeof(DirNode));
        if (0 == strcmp(dir->dirname,pathname))
        {
            return i;
        }
    }
    return -1;
}

static int GetDirIdx(DirNode * dir)
{
    char *diraddr = (char *)dir;
    int diridx= ((unsigned char*)diraddr - g_player_info.dirarray.list)/sizeof(DirNode);
    if (diridx >= g_player_info.dirarray.listidx)
    {
        log("GetDirIdx %d > %d\n",diridx,g_player_info.dirarray.listidx);
        return -1;
    }
    return diridx;
}

static DirNode *GetDirNodeByName(char *pathname)
{
    int i;
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;
    DirNode *dir = NULL;
    if (NULL == pathname)
        return NULL;
    
    for (i = 0;i < idx;i++)
    {
        dir = (DirNode*)(dirarray->list + i * sizeof(DirNode));
        if (0 == strcmp(dir->dirname,pathname))
        {
            return dir;
        }
    }
    return NULL;
}

static SingleMedia *GetAudioByDirIdx(DirNode *dir,int aididx)
{
    SingleMedia *tmpaudio = NULL;
    int audioidx = dir->audiolist[aididx];
    if (aididx >= dir->audionum)
    {
        log("GetAudioByDirIdx faild %d > %d\n",aididx,dir->audionum);
        return NULL;
    }
    tmpaudio = GetAudioFileByIdx(dir,audioidx);
    if (tmpaudio != NULL)
    {
        /*将audioid 赋值，默认是没有值的，此为专门为广告插件做的需求*/
        //strncpy(tmpaudio->audioid,dir->audioid[aididx],MAX_AUDIO_ID);
    }
    return tmpaudio;
}

DirNode *GetDirNodeByIdx(int aididx,int reload)
{
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;
    DirNode *dir = NULL;
    if (aididx >= idx)
    {
        log("GetDirNodeByIdx faild %d > %d\n",aididx,idx);

        return NULL;
    }
    
    dir = (DirNode*)(dirarray->list + aididx * sizeof(DirNode));
    if (reload == 1)
    {
        LoadDirFile(dir,0);
    }
    
    return dir;
}

void UpdateCurMediaInfo(SingleMedia *media,int curtime)
{
    if (media == NULL)
    {
        memset(&(g_player_info.curaudioinfo),0,sizeof(CurAudioInfo));
    }
    else
    {
        strcpy(g_player_info.curaudioinfo.curdir,media->dirname);
        strcpy(g_player_info.curaudioinfo.curaudio,media->medianame);
        g_player_info.curaudioinfo.media = media;

        g_player_info.curaudioinfo.curtime = curtime;
        memset(g_player_info.curaudioinfo.taginfo,0,128);
    }
}

void SetCurMediaCurTime(int curtime)
{
    g_player_info.curaudioinfo.curtime = curtime;
}

void SetCurMeidaCurDir(char *curdir)
{
    strcpy(g_player_info.curaudioinfo.curdir,curdir);
}

void SetCurMediaCurAudio(char *curaudio)
{
    strcpy(g_player_info.curaudioinfo.curaudio,curaudio);
}

void SetCurMediaTotalTime(int totaltime)
{
    g_player_info.curaudioinfo.totaltime = totaltime;
}

char *GetCurMediaName(void)
{
    return g_player_info.curaudioinfo.curaudio;
}

SingleMedia *GetCurSingleMedia(void)
{
    return g_player_info.curaudioinfo.media;
    
}

char *GetCurDirName(void)
{
    return g_player_info.curaudioinfo.curdir;
}

int GetCurTime(void)
{
    return g_player_info.curaudioinfo.curtime;
}


static int PrevDirIdx(DirNode * dir)
{
    int diridx = GetDirIdx(dir);
    if (diridx == -1)
    {
        log("GetDirIdx failed\n");
        return -1;
    }
    
    diridx--;
    
    if (diridx < 0)
    {
        diridx = g_player_info.dirarray.listidx - 1;
    }
    return diridx;
}

typedef struct RandomList_S{
    int randomidxlist[100];
    int lastidx;
}RandomList;

static RandomList RandIdxList={
    .lastidx = 0,
};

static int GetRandomNext(int rang)
{
    int aididx = 0;
    int random = 0;
    srand(time(NULL));
    random = rand();
    aididx = (random % rang);
    //printf("aididx=%d %d %d %d\n",aididx,nextdir->audionum,random,rand());
    rand();
    
    /*record random list for preview*/
    RandIdxList.randomidxlist[RandIdxList.lastidx] = aididx;
    RandIdxList.lastidx++;
    if (RandIdxList.lastidx == 100)
    {
       RandIdxList.lastidx = 0; 
    }
    return aididx;
}

static int GetRandomPrev()
{
    int previdx;
    if (RandIdxList.lastidx == 0)
        return -1;
        
    /*表示当前播放的歌*/
    RandIdxList.lastidx--;
    if (RandIdxList.lastidx == 0)
    {
        /*没有上一首，返回当前歌*/
        return RandIdxList.randomidxlist[0];
       //RandIdxList.lastidx = 1; 
    }
    /*表示当前播放的上一首歌*/
    previdx = RandIdxList.lastidx - 1;
    return RandIdxList.randomidxlist[previdx];
}

static void ClearRandomList()
{
    memset(&RandIdxList,0,sizeof(RandIdxList));
}

static int NextDirIdx(DirNode * dir)
{
    int diridx = GetDirIdx(dir);
    if (diridx == -1)
    {
        log("GetDirIdx failed\n");
        return -1;
    }
    diridx++;
    if (diridx >= g_player_info.dirarray.listidx)
    {
        diridx = 0;
    }
    
    return diridx;
}

/*指定目录中的下一首，用于手动调节*/
static SingleMedia *FindNextAudioInDir(DirNode * dir)
{
    int diridx;
    int playmode;
    DirNode *nextdir = dir;
    SingleMedia *nextaudio = NULL;
    int aididx = nextdir->aididx;

    playmode = g_player_info.playmode;
    /*mediatype == audio则继续*/
    if (nextdir->mediatype == MEDIA_AUDIO)
    {
        playmode = PLAY_MODE_LOOP;
    }
    if (nextdir->seqorder == 0)
    {
        if (playmode == PLAY_MODE_RAMDOM)
        {
            aididx = GetRandomNext(nextdir->audionum);
        }
        else
        {
            log("aididx=%d nextdir->audionum=%d\n",aididx,nextdir->audionum);
            aididx++;
            //log("aididx=%d nextdir->audionum=%d\n",aididx,nextdir->audionum);
            if (aididx >= nextdir->audionum)
            {
               
               aididx = 0;
               log("aididx=%d\n",aididx);
            }
        }
        nextdir->aididx = aididx;
    }
    else if (nextdir->seqorder == 1)
    {
        if (playmode == PLAY_MODE_RAMDOM)
        {
            aididx = GetRandomPrev();
            /*random list 为空，依然播放当前歌曲*/
            if (aididx  == -1)
            {
                aididx =nextdir->aididx;
            }
        }
        else
        {
            aididx--;
            if (aididx < 0)
            {
               //strcpy(g_player_info.curaudio,"");
               //g_player_info.curaudioinfo.media = NULL;
               //return NULL;
               aididx = nextdir->audionum - 1;
            }
        }
        nextdir->aididx = aididx;
    }
    
    log("netx idx=%d\n",nextdir->aididx);
    
    nextaudio = GetAudioByDirIdx(nextdir,nextdir->aididx);
    
    
    /*audio info*/
    //printf("nextaudio:%s\n",nextaudio->medianame);
    UpdateCurMediaInfo(nextaudio,0);

    return nextaudio;
}

static SingleMedia *FindNextDirAudio(DirNode * dir)
{
    int diridx;
    DirNode *nextdir = dir;
    SingleMedia *nextaudio = NULL;
    int aididx = nextdir->aididx;
    int playmode = 0;

    /*如果收到广告消息，则播放广告，同时关闭按键功能*/
    if (g_adaudio.enable == 1)
    {
        g_player_info.button.disabled = 1;
        UpdateCurMediaInfo(&(g_adaudio.media),0);
        return &(g_adaudio.media);
    }
#if 0
    /*如果正在同步数据,暂时播放当前歌曲*/
    if (g_player_info.syncstatus == AUDIO_SYNC_PROCESS)
    {
        //nextdir->aididx = aididx;
        nextaudio = GetAudioByDirIdx(nextdir,aididx);
    
        /*audio info*/
        //printf("nextaudio:%s\n",nextaudio->medianame);
        UpdateCurMediaInfo(nextaudio,0);
        
        //UpdataConfig(); 
        UpdateStatus();
        return nextaudio;
    }
#endif
    playmode = g_player_info.playmode;
    
    /*根据多听要求，当前的专辑是audio时，
    不受播放模式的影响，只是用DIR LOOP模式*/
    if (dir->mediatype == MEDIA_AUDIO)
    {
        playmode = PLAY_MODE_DIR_LOOP;
    }

    //int seqorder = nextdir->seqorder;
    if (PLAY_MODE_SEQUENCE == playmode)
    {
        aididx++;
        if (aididx >= nextdir->audionum)
        {
           UpdateCurMediaInfo(NULL,0);
           return NULL;
        }
        nextdir->aididx = aididx;
    }
    else if (PLAY_MODE_SINGLE == playmode)
    {
        nextdir->aididx = aididx;
    }
    else if (PLAY_MODE_LOOP == playmode)
    {
        /*如果本目录没有歌曲，自动下个目录*/
        if (nextdir->audionum == 0)
        {
            diridx = NextDirIdx(nextdir);
            //printf("diridx=%d\n",diridx);
            if (diridx == -1)
            {
                 diridx = 0;
            }
            g_player_info.dirindex = diridx;
            
            nextdir = GetDirNodeByIdx(diridx,1);
            if (nextdir == NULL)
            {
                log("not exist album \n");
                return NULL;
            }
            nextdir->aididx = aididx = 0;
            g_player_info.switch_dir = 1;
        }
        else
        {
            aididx++;
            if (aididx >= nextdir->audionum)
            {
               //strcpy(g_player_info.curaudio,"");
               //g_player_info.curaudioinfo.media = NULL;
               //return NULL;
               aididx = 0;
            }
            nextdir->aididx = aididx;
        }
    }
    else if (PLAY_MODE_RAMDOM == playmode)
    {
        aididx = GetRandomNext(nextdir->audionum);
        nextdir->aididx = aididx;

    }
    else if (PLAY_MODE_DIR_SEQ == playmode)
    {
        aididx++;
        
        if (aididx >= nextdir->audionum)
        {
            diridx = NextDirIdx(nextdir);
            //printf("diridx=%d\n",diridx);
            if (diridx == -1)
            {
                log("Get Next Dir Index Failed\n");
                UpdateCurMediaInfo(NULL,0);
                return NULL;
            }
            g_player_info.dirindex = diridx;
            
            nextdir = GetDirNodeByIdx(diridx,1);
            if (nextdir == NULL)
            {
                log("not exist album \n");
                return NULL;
            }
            nextdir->aididx = aididx = 0;
            
        }
        nextdir->aididx = aididx;
    }
    else if (PLAY_MODE_DIR_LOOP == playmode)
    {
        aididx++;
        
        if (aididx >= nextdir->audionum)
        {
            diridx = NextDirIdx(nextdir);
            if (diridx == -1)
            {
                //printf("Get Next Dir Index Failed\n");
                //UpdateCurMediaInfo(NULL,0);
                //return NULL;
                
                diridx = 0;
            }
            g_player_info.dirindex = diridx;
            
            nextdir = GetDirNodeByIdx(diridx,1);
            if (nextdir == NULL)
            {
                log("not exist album \n");
                return NULL;
            }
            nextdir->aididx = aididx = 0;
            g_player_info.switch_dir = 1; /*为了让下个专辑能播报专辑名*/
        }
        nextdir->aididx = aididx;
    }
    
    nextaudio = GetAudioByDirIdx(nextdir,aididx);

    /*audio info*/
    //printf("nextaudio:%s\n",nextaudio->medianame);
    UpdateCurMediaInfo(nextaudio,0);
    
    //UpdataConfig(); 
    UpdateStatus();

    return nextaudio;

}

/*仅仅增加目录，不增加目录下的歌曲*/
static int AddDirNullAlbum(char *path,char *album)
{
    int ret;
    int i;
    int aid;
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;
    DirNode *dir = NULL;

    //log("AddDirNullAlbum %s audioidx=%s\n",path,album);
    for (i = 0;i < idx;i++)
    {
        dir = (DirNode*)(dirarray->list + i * sizeof(DirNode));
        if (0 == strcmp(dir->dirname,path))
        {
            //log("have add %s\n",dir->dirname);
            return SUCCESS;
        }
    }
    /*这里是新建目录的分支*/
    
    dir = (DirNode*)(dirarray->list + idx * sizeof(DirNode));
    dir->audionum = 0;
    dir->mediatype = GetMeidaType(path);
    dir->audiolist[0] = -1;
    strcpy(dir->dirname,path);
    strcpy(dir->albumname,album);
    //SetDirValid(dir);
    dir->valid = 0;
    g_player_info.dirarray.listidx++;
    if (g_player_info.dirarray.listidx >= MAX_DIR_NUM)
    {
        log("reach max dir num\n");
        return -1;
    }   
    return SUCCESS;

}

/*增加目录，并同时增加目录下的歌曲顺序*/
static int AddDirIdx(char *path,char *album,int audioidx,int mediatype,DirNode **dnode)
{
    int ret;
    int i;
    int aid;
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;
    DirNode *dir = NULL;
    
    for (i = 0;i < idx;i++)
    {
        dir = (DirNode*)(dirarray->list + i * sizeof(DirNode));
        if (0 == strcmp(dir->dirname,path))
        {
            //log("AddDirIdx %s audioidx=%d\n",path,audioidx);
            for (aid = 0;aid < dir->audionum;aid++)
            {
                /*已经存在当前的audioidx*/
                if (dir->audiolist[aid] == audioidx)
                {
                    //log("audio %d aready in %s\n",audioidx,path);
                    return SUCCESS;
                }
            }
            dir->audiolist[dir->audionum] = audioidx;
            dir->mediatype = mediatype;
            dir->audionum++;
            if (dir->audionum > MAX_AUDIO_INDIR_NUM)
            {
                log("audionum too max %d\n",dir->audionum);
                return -1;
            }
            //*dnode = dir;
            //g_player_info.dirarray.listidx++;
            return SUCCESS;
        }
    }
    /*这里是新建目录的分支*/

    dir = (DirNode*)(dirarray->list + idx * sizeof(DirNode));
    dir->audionum = 0;
    strcpy(dir->dirname,path);
    strcpy(dir->albumname,album);
    dir->audiolist[dir->audionum] = audioidx;
    dir->mediatype = mediatype;
    dir->audionum++;
    if (dir->audionum > MAX_AUDIO_INDIR_NUM)
    {
        log("audionum too max %d\n",dir->audionum);
        return -1;
    }
    dir->audiolist[dir->audionum] = -1;
    //*dnode = dir;
    g_player_info.dirarray.listidx++;
    if (g_player_info.dirarray.listidx >= MAX_DIR_NUM)
    {
        log("reach max dir num\n");
        return -1;
    }  
    return SUCCESS;
    
}



static int GetAudioPlayTimes(char *dir,char *file)
{
    
    return SUCCESS;
}

static int GetMeidaType(char *path)
{
    char tmpfile[MAX_DIR_NAME] = {0};
    int len = 0;
    int fd = -1;
    char *buffer = NULL;
    char *tmptype = NULL;
    int mediatype = 0;
    struct xmlelement *type_node;
    struct xmlelement *path_node;
    struct xmlelement *root;
    struct xmldoc *doc = NULL;
    IXML_Node * node = NULL;
    
    strcpy(tmpfile,path);
    
    /*   增加/  */
    //if (tmpfile[strlen(tmpfile) - 1] != '/')
    //    tmpfile[strlen(tmpfile)] = '/';
        
    strcat(tmpfile,"album_info.xml");
    if((fd=open(tmpfile,O_RDONLY))<0){
            log("%s open file error\n",path);
            return MEDIA_MUSCI; 
    }

    len=lseek(fd,0,SEEK_END); 
    
    buffer=mmap(NULL,len,PROT_READ,MAP_SHARED,fd,0);//读写得和open函数的标志相一致，否则会报错
    if(buffer==MAP_FAILED){
        log("mmap error\n");
        close(fd);
        return MEDIA_MUSCI; 
    }
    
    doc = xmldoc_parsexml(buffer);
    if (doc == NULL)
    {
        log("xml parse error\n");
        munmap(buffer,len);
        close(fd);
        return MEDIA_MUSCI;
    }
    
    //node = to_idoc(doc);
    root = find_element_in_doc(doc,"album");
    if (root == NULL)
    {
        log("xml parse error\n");
        munmap(buffer,len);
        close(fd);
        xmldoc_free(doc);
        return MEDIA_MUSCI;
    }
    node=to_ielem(root);
    /*get media type*/
    type_node = find_element_in_element(node,"type");
    if (type_node == NULL)
    {
        log("xml type parse error\n");
        munmap(buffer,len);
        close(fd);
        xmldoc_free(doc);
        return MEDIA_MUSCI;
    }
    
    tmptype=get_node_value(type_node);
    mediatype = atoi(tmptype);

    xmldoc_free(doc);
    munmap(buffer,len);
    close(fd);
    
    return mediatype;
}

SingleMedia audiomedia;
int FindAudioIdxInCache(char *dir,char *medianame)
{
    int i = 0;
    int audioidx = 0;
    char path[MAX_DIR_NAME],album[MAX_ALBUM_NAME],filename[MAX_MEDIA_NAME],mediatype[4];
    if (audiolist.validnum == 0)
        return -1;
    
    for (i = 0;i < audiolist.validnum;i++)
    {

         sscanf(audiolist.line[i],"%[^&]&%[^&]&%[^&]&%[^\n]",path,album,filename,mediatype);

         if(strcmp(path,dir) != 0)
            continue;
        
        /*找到对应文件所在的行*/
        if (strcmp(filename,medianame)==0)
        {
            //return linenum;
            audioidx = i + audiolist.startline;
            //log("find idx %d in cache %s %s\n",audioidx,dir,medianame);
            return audioidx;
        }
    }
    return -1;
}

int IsInAudioCache(int idx)
{
    if (audiolist.validnum == 0)
        return 0;
    if (idx >= audiolist.startline
    && idx <= audiolist.endline)
    {
        return 1;
    }
    return 0;
}

void ClearACache()
{
    audiolist.validnum = 0;
    audiolist.startline= -1;
    audiolist.endline= -1;
}

int UpgradeNewACahche(DirNode *dirnode,int start)
{
    FILE *fp = NULL;
    char tmpbuffer[512] = {0};
    char cmd[256] = {0};
    int num = 0;
    int numincahce = 0;
    ClearACache();

    sprintf(cmd,"%s/%s",MUSIC_SYSCONFIG_PATH,dirnode->albumname);
    fp = fopen(cmd,"r");
    if (fp == NULL)
    {
        log("system error,maybe disk lost or file not exist\n");
        return -1;
    }
    
    while (feof(fp) == 0)
    {
        memset(tmpbuffer,0,512);
        fgets(tmpbuffer,512,fp);
        if (num >= start)
        {
            
            strncpy(audiolist.line[audiolist.validnum],tmpbuffer,512);
    
            audiolist.validnum++;
            if (audiolist.validnum >= MAX_LINE_IN_CACHE)
            {
                break;
            }
        }
        num++;
    }

    audiolist.startline = start;
    audiolist.endline = start + audiolist.validnum;
    
    fclose(fp);
    return 0;
}


static int AddAudioFile(char *path,char *album,char *filename,int mediatype)
{
    FILE *fp = NULL;
    char tmpbuffer[512] = {0};
    fp = fopen(MUSIC_AUDIOLIST_FILE,"a+");
    if (fp == NULL)
    {
        log("system error,maybe disk lost or full\n");
        return -1;
    }
    sprintf(tmpbuffer,"%s&%s&%s&%d\n",path,album,filename,mediatype);
    fputs(tmpbuffer,fp);
    fclose(fp);
    AddDirNullAlbum(path,album);
    return 0;
}



#define SPLITE_DIR_LOAD
#ifdef SPLITE_DIR_LOAD

static void SetDirValid(DirNode *dirnode)
{
    dirnode->valid = 1;
}

static void SetDirInvalid(DirNode *dirnode)
{
    char cmd[256] = {0};
    char filename[256] = {0};
    sprintf(filename,"%s/%s",MUSIC_SYSCONFIG_PATH,dirnode->albumname);

    if (access(filename,F_OK)==0)
    {
        sprintf(cmd,"rm -rf \"%s\"",filename);
        system(cmd);
    }
    dirnode->valid = 0;
}
#if 0
#define DIR_NOTIFY_FLAGS  (IN_CREATE | IN_DELETE)
static int AddDirWatch(DirNode *dirnode)
{
    int ret = 0;
    if (dirnode->wtd > 0)
    {
        return 0;
    }
    ret = inotify_add_watch(g_player_info.dirwtd,dirnode->dirname,DIR_NOTIFY_FLAGS);
    if (ret < 0)
    {
        return -1;
    }
    dirnode->wtd = ret;
    return 0;
}
static void RemoveDirWatch(DirNode *dirnode)
{
    if (dirnode->wtd > 0)
    {
        inotify_rm_watch(g_player_info.dirwtd,dirnode->wtd);
        dirnode->wtd = -1;
    }
}

static int AddAllDirWatch()
{
    int i;
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;
    DirNode *dir = NULL;

    for (i = 0;i < idx;i++)
    {
        dir = (DirNode*)(dirarray->list + i * sizeof(DirNode));
        AddDirWatch(dir);
    }
    return 0;
}

static int RemoveAllDirWatch()
{
    int i;
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;
    DirNode *dir = NULL;

    for (i = 0;i < idx;i++)
    {
        dir = (DirNode*)(dirarray->list + i * sizeof(DirNode));
        RemoveDirWatch(dir);
    }
    
    return 0;
}
#endif

static void setAllDirInvalid()
{
    int i;
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;
    DirNode *dir = NULL;
    
    for (i = 0;i < idx;i++)
    {
        dir = (DirNode*)(dirarray->list + i * sizeof(DirNode));
        SetDirInvalid(dir);
    }
}


static int GetDirValid(DirNode *dirnode)
{
    return dirnode->valid;
}

/*
static int AddDirWatch(DirNode *dirnode)
{
    dirnode->wfd = 
}
*/

static int GetAudioIndexByName(DirNode *dirnode,char *medianame)
{
    FILE *fp = NULL;
    int linenum = 0;
    int firstdirnum = -1;
    char tmpbuffer[1024] = {0};
    char cmd[256] = {0};
    char path[MAX_DIR_NAME],album[MAX_ALBUM_NAME],filename[MAX_MEDIA_NAME],mediatype[4];

    /*
    linenum = FindAudioIdxInCache(dirnode,medianame);
    if (linenum >= 0)
        return linenum;
    */
    sprintf(cmd,"%s/%s",MUSIC_SYSCONFIG_PATH,dirnode->albumname);
    fp = fopen(cmd,"r");
    if (fp == NULL)
    {
        log("file %s not exist\n",cmd);
        return -1;
    }
    linenum = 0;
    while (feof(fp) == 0)
    {
        fgets(tmpbuffer,1024,fp);
        if (strlen(tmpbuffer) == 0)
        {
            log("no audio in file list %d\n",linenum);
            fclose(fp);
            return -1;
        }
        
        /*如果当前行已经在cache中了，就不需要再去比较*/
        /*
        if (IsInAudioCache(linenum))
        {
            linenum++;
            continue;
        }
        */

        sscanf(tmpbuffer,"%[^&]&%[^&]&%[^&]&%[^\n]",path,album,filename,mediatype);
        if(0 != strcmp(path,dirnode->dirname))
        {
            linenum++;
            continue;
        }
        /*记录此目录第一个文件的序号，用于
             加载到cache中*/
        //if (firstdirnum == -1)
        //    firstdirnum = linenum;
        
        /*找到对应文件所在的行*/
        if (strcmp(filename,medianame)==0)
        {
            fclose(fp);
            /*因为目录中的文件按顺序排着，所以预先加载1024个此目录的文件*/
            //if (firstdirnum != -1)
            //    UpgradeNewACahche(dirnode,firstdirnum);
            return linenum;
        }

        linenum++;
    }
    fclose(fp);
    
    return -1;
}


int LoadAudioFromCache(DirNode *dirnode,int idx)
{
    char path[MAX_DIR_NAME],album[MAX_ALBUM_NAME],filename[MAX_MEDIA_NAME],mediatype[4];
    if (audiolist.validnum == 0)
        return -1;
    
    if (idx >= audiolist.startline
    && idx <= audiolist.endline)
    {
        sscanf(audiolist.line[idx - audiolist.startline],
        "%[^&]&%[^&]&%[^&]&%[^\n]",path,album,filename,mediatype);
        memset(&audiomedia,0,sizeof(audiomedia));
        strcpy(audiomedia.albumname,album);
        strcpy(audiomedia.dirname,path);
        strcpy(audiomedia.medianame,filename);
        audiomedia.mediatype = atoi(mediatype);
        audiomedia.playtimes = 0;
        audiomedia.dir = NULL;
        //log("find idx  %d %s %s in cache\n",idx,audiomedia.dirname,audiomedia.medianame);
        return 0;
    }
    return -1;
}

static SingleMedia *GetAudioFileByIdx(DirNode *dirnode,int idx)
{
    FILE *fp = NULL;
    char cmd[256];
    int num = 0;
    int ret = 0;
    char tmpbuffer[512] = {0};
    char path[MAX_DIR_NAME],album[MAX_ALBUM_NAME],filename[MAX_MEDIA_NAME],mediatype[4];
    char filetmp[MAX_DIR_NAME] = {0};
    /*
    ret = LoadAudioFromCache(dirnode,idx);
    if (ret == 0)
    {
        return &audiomedia;
    }
    */
    
    log("GetAudioFileByIdx dirnode=%s %d\n",dirnode->dirname,idx);
    
    sprintf(cmd,"%s/%s",MUSIC_SYSCONFIG_PATH,dirnode->albumname);
    fp = fopen(cmd,"r");
    if (fp == NULL)
    {
        log("maybe disk lost or %s not exist\n",cmd);
        return NULL;
    }

    while (feof(fp) == 0)
    {
        memset(tmpbuffer,0,512);
        fgets(tmpbuffer,512,fp);
        if (num == idx)
        {
            
            sscanf(tmpbuffer,"%[^&]&%[^&]&%[^&]&%[^\n]",path,album,filename,mediatype);
            sprintf(filetmp,"%s%s",path,filename);
            /*当前播放文件不存在，可能需要重新载入*/
            if (access(filetmp,F_OK) != 0)
            {
                /**/
                log("file %s not exist\n",filetmp);
                //SetDirInvalid(dirnode);
                LoadDirFile(dirnode,1);
                return NULL;
            }
            
            memset(&audiomedia,0,sizeof(audiomedia));
            strcpy(audiomedia.albumname,album);
            strcpy(audiomedia.dirname,path);
            strcpy(audiomedia.medianame,filename);
            audiomedia.mediatype = atoi(mediatype);
            //log("audiomedia.mediatype=%d\n",audiomedia.mediatype);
            audiomedia.playtimes = 0;
            audiomedia.dir = NULL;
            
            fclose(fp);
            return &audiomedia;   
        }
        num++;
    }
    //sprintf(tmpbuffer,"%s&%s&%s&%d\n",path,album,filename,mediatype);
    
    fclose(fp);
    return NULL;
    
    
}


int AddAudioToDirlist(DirNode *dirnode,char *audioname,int mediatype)
{
    FILE *fp = NULL;
    char cmd[256] = {0};
    char tmpbuffer[1024] = {0};
    sprintf(cmd,"%s/%s",MUSIC_SYSCONFIG_PATH,dirnode->albumname);
    fp = fopen(cmd,"a+");
    if (fp == NULL)
    {
        log("system error,maybe disk lost or full\n");
        return -1;
    }
    sprintf(tmpbuffer,"%s&%s&%s&%d\n",dirnode->dirname,dirnode->albumname,audioname,mediatype);
    fputs(tmpbuffer,fp);
    fclose(fp);
    return 0;
}


/*增加目录，并同时增加目录下的歌曲顺序*/
static int AddtoDirIdx(DirNode *dirnode,int audioidx,char *audioid)
{
    int ret;
    int i;
    int aid;
    Array *dirarray = &(g_player_info.dirarray);
    int idx = g_player_info.dirarray.listidx;

    //log("AddDirIdx %s audioidx=%d\n",path,audioidx);
    for (aid = 0;aid < dirnode->audionum;aid++)
    {
        /*已经存在当前的audioidx*/
        if (dirnode->audiolist[aid] == audioidx)
        {
            //log("audio %d aready in %s\n",audioidx,path);
            return SUCCESS;
        }
    }
    //log("dirnode=%s audionum=%d audioidx=%d\n",dirnode->albumname,
    //dirnode->audionum,audioidx);
    
    //strncpy(dirnode->audioid[dirnode->audionum],audioid,16);
    
    dirnode->audiolist[dirnode->audionum] = audioidx;
    dirnode->audionum++;
    if (dirnode->audionum > MAX_AUDIO_INDIR_NUM)
    {
        log("audionum too max %d\n",dirnode->audionum);
        return -1;
    }

    return SUCCESS;

}



static int ScanDirFile(DirNode *dirnode)
{
    DIR *dp;          //目录流
    struct dirent *entry;     //目录项信息
    struct stat statbuf;
    int mediatype = GetMeidaType(dirnode->dirname);
//    log("dirnode->dirname =%s mediatype=%d\n",dirnode->dirname,mediatype);
    //打开目录, 判断目录是否存在
    if ((dp = opendir (dirnode->dirname)) == 0)
    {
        log ("open dir failed::: %s\n ", dirnode->dirname);
        perror("");
        return -1;
    }

    //读取目录信息
    while ((entry = readdir(dp)) != 0)
    {
        //忽略./..目录
        if (!strncmp (entry->d_name, ".",1) || !strncmp (entry->d_name, "..",2))
        {
            continue;
        }

        if (!strcmp (entry->d_name, "lost+found"))
        {
            continue;
        }

        //获取扫描到的文件的信息, 如果路径中没有'/', 则加'/', 加入strvec
        //不管是目录,还是文件,都将被加进去.

        //printf("%s\n",tmppath);

        if (CheckValidAudioFile(entry->d_name))
        {
            LoadFirstPlay(dirnode,entry->d_name,0,1);
            /*首先播放第一首找到的歌曲*/
            //log("dirnode=%s audio=%s mediatype=%d\n",dirnode->dirname, entry->d_name,mediatype);
            AddAudioToDirlist(dirnode,entry->d_name,mediatype);
        }


    }
    closedir (dp);

    return 0;
}

static int AddXmlAudioToDir(DirNode *dir)
{
    int i = 0;
    int maxdir = g_player_info.dirarray.listidx;
    int ret = 0;
    Array *dirarray = &(g_player_info.dirarray);
    char tmppath[MAX_DIR_NAME] = {0};
    char *buffer = NULL;
    char *medianame = NULL;
    char *tmptype = NULL;
    int mediatype = 0;
    struct xmlelement *type_node;
    struct xmlelement *path_node;
    struct xmlelement *id_node;
    struct xmlelement *root;
    struct xmldoc *doc = NULL;
    IXML_Node * node;
    
    int fd = -1;
    int len = 0;
    
    if (GetDirValid(dir))
    {
        //log("dir %s valid\n",dir->albumname);
        return 0;
    }
    //strcpy(tmppath,rootpath);
    memset(tmppath,MAX_DIR_NAME,0);
    strcpy(tmppath,dir->dirname);
    strcat(tmppath,"album_info.xml");
    if((fd=open(tmppath,O_RDONLY))<0){
        log("open file error\n");

        //return -1;
        return 0; /*如果没有album_info继续读真实文件*/
    }

    len=lseek(fd,0,SEEK_END); 
    
    buffer=mmap(NULL,len,PROT_READ,MAP_SHARED,fd,0);//读写得和open函数的标志相一致，否则会报错
    if(buffer==MAP_FAILED){
        log("mmap error\n");
        close(fd);

        //return -1;
        return 0;/*如果没有album_info继续读真实文件*/
    }
    
    doc = xmldoc_parsexml(buffer);
    if (doc == NULL)
    {
        log("xml parse error\n");
        munmap(buffer,len);
        close(fd);
        //return -1;
        return 0;/*如果没有album_info继续读真实文件*/
    }
    //node = to_idoc(doc);
    root = find_element_in_doc(doc,"album");
    if (root == NULL)
    {
        log("album not found\n");
        munmap(buffer,len);
        close(fd);
        xmldoc_free(doc);
        //return -1;
        return 0;/*如果没有album_info继续读真实文件*/
    }
    node=to_ielem(root);
    /*get media type*/
    type_node = find_element_in_element(node,"type");
    if (type_node == NULL)
    {
        log("type not found\n");
        munmap(buffer,len);
        close(fd);
        xmldoc_free(doc);
        //return -1;
        return 0;/*如果没有album_info继续读真实文件*/
    }
    
    tmptype=get_node_value(type_node);
    mediatype = atoi(tmptype);
    
    node=find_element_in_element(node,"track_list");
    //node = to_idoc(node);
    node = ixmlNode_getFirstChild(node);
	for (/**/; node != NULL; node = ixmlNode_getNextSibling(node)) {
		if (strcmp(ixmlNode_getNodeName(node), "track") == 0) {
		    int idx = 0;
		    char *audioid = NULL;
		    //SingleMedia *audio;
			//return (struct xmlelement*) node;
            path_node = find_element_in_element(node,"path");
            if (path_node == NULL)
            {
                continue;
            }
            
            medianame=get_node_value(path_node);
            
            
            //log("medianame=%s\n",medianame);
            idx = GetAudioIndexByName(dir,medianame);
            if (idx != -1)
            {
                id_node = find_element_in_element(node,"id");
                //audioid = get_node_value(id_node);
                audioid = "-1";
                //log("add xml info audio=%s %d\n",medianame,idx);
                ret = AddtoDirIdx(dir,idx,audioid);
                if (ret != SUCCESS)
                {
                    log("AddDirIdx failed \n");
                    //close(fd);
                    break;
                }
            }
		}
	}
    xmldoc_free(doc);
	munmap(buffer,len);
    close(fd);
    
    return 0;
}

static int AddDiskAudiotoDir(DirNode *dirnode)
{
    FILE *fp = NULL;
    int linenum = 0;
    int ret;
    char cmd[256] = {0};
    char tmpbuffer[512] = {0};
    char path[MAX_DIR_NAME],album[MAX_ALBUM_NAME],filename[MAX_MEDIA_NAME],mediatype[4];
    
    if (GetDirValid(dirnode))
    {
        //log("dir %s valid\n",dirnode->albumname);
        return 0;
    }

    sprintf(cmd,"%s/%s",MUSIC_SYSCONFIG_PATH,dirnode->albumname);
    fp = fopen(cmd,"r");
    if (fp == NULL)
    {
        log("file %s not exist\n",cmd);
        return -1;
    }
    while (feof(fp) == 0)
    {
        memset(tmpbuffer,0,512);
        fgets(tmpbuffer,512,fp);
        if (strlen(tmpbuffer) == 0)
        {
            //log("no audio in file list %d\n",linenum);
            fclose(fp);
            return 0;
        }
        sscanf(tmpbuffer,"%[^&]&%[^&]&%[^&]&%[^\n]",path,album,filename,mediatype);
        /*找到对应文件所在的行*/
        ret = AddtoDirIdx(dirnode,linenum,"-1");
        if (ret != SUCCESS)
        {
            log("AddDirIdx failed \n");
            fclose(fp);
            return -1;
        }
        linenum++;
    }
    fclose(fp);
    
    return 0;
}


int AddAllFileToDir(DirNode * dir)
{
    int ret = 0;
    ret = AddXmlAudioToDir(dir);
    if (ret != 0)
    {
        return -1;
    }

    ret = AddDiskAudiotoDir(dir);
    if (ret != 0)
    {
        return -1;
    }
    SetDirValid(dir);
    return 0;
}



void ClearDirInfo(DirNode *dirnode)
{
    dirnode->audionum = 0;
    dirnode->aididx = 0;
    dirnode->valid = 0;
}


int LoadDirFile(DirNode * dirnode,int scandisk)
{
    int ret  = 0;
    char cmd[256] = {0};
    char filename[512] = {0};
    if (scandisk == 1)
    {
        /*
        sprintf(filename,"%s/%s",MUSIC_SYSCONFIG_PATH,dirnode->albumname);
        if (access(filename,F_OK)==0)
        {
            
            sprintf(cmd,"rm -rf %s",filename);
            system(cmd);
        }
        */
        SetDirInvalid(dirnode);

        ret = ScanDirFile(dirnode);
        if (0 != ret)
        {
            log("ScanAudioFile failed\n");
            ClearDirInfo(dirnode);
            return -1;
        }
        ClearDirInfo(dirnode);
        dirnode->scaned = 1;
    }
    else
    {
        sprintf(filename,"%s/%s",MUSIC_SYSCONFIG_PATH,dirnode->albumname);
        //log("LoadDirFile:%s\n",filename);
        if (access(filename,F_OK)!=0)
        {
            ret = ScanDirFile(dirnode);
            if (0 != ret)
            {
                log("ScanAudioFile failed\n");
                ClearDirInfo(dirnode);
                return -1;
            }
            ClearDirInfo(dirnode);
            dirnode->scaned = 1;
        }
    }
    system("sync");
    
    AddAllFileToDir(dirnode);
    
    return ret;

}

int AddRealAudioDir(char *path)
{
    DIR *dp;          //目录流
    int ret = 0;
    struct dirent *entry;     //目录项信息
    struct stat statbuf;
    char tmppath[MAX_DIR_NAME] = {0};
    char tmpxmlname[MAX_DIR_NAME] = {0};
    //int mediatype = GetMeidaType(dirnode->dirname);
    
    if ((dp = opendir (path)) == 0)
    {
        fprintf (stderr, "open dir failed::: %s\n ",path);
        perror("");
        return -1;
    }
    
    //读取目录信息
    while ((entry = readdir(dp)) != 0)
    {
        //忽略./..目录
        if (!strncmp (entry->d_name, ".",1) || !strncmp (entry->d_name, "..",2))
        {
            continue;
        }

        if (!strcmp (entry->d_name, "lost+found"))
        {
            continue;
        }

        if (entry->d_type == 4)
        {
            strcpy(tmppath,path);
            /*   增加/  */
            if (tmppath[strlen(tmppath) - 1] != '/')
                tmppath[strlen(tmppath)] = '/';
            strcat(tmppath,entry->d_name);
            
            /*   增加/  */
            if (tmppath[strlen(tmppath) - 1] != '/')
                tmppath[strlen(tmppath)] = '/';

            strcpy(tmpxmlname,tmppath);
            strcat(tmpxmlname,"album_info.xml");
            if (0 == access(tmpxmlname,F_OK))
            {
                //log("AddRealAudioDir:%s %s\n",tmppath,entry->d_name);
                AddDirNullAlbum(tmppath,entry->d_name);
            }
        }

        memset(tmppath,0,MAX_DIR_NAME);
    }
    closedir (dp);
    

}

void LoadFirstPlay(DirNode *dirnode,char *media,int time,int now)
{
    SingleMedia tmpsingleMedia; 
    if (GetPlayStatus() == MPLAYER_STOP && now && dirnode->mediatype != MEDIA_AUDIO)
    {
        log("LoadFirstPlay:%s %s\n",dirnode->albumname,media);
        memset(&tmpsingleMedia,0,sizeof(tmpsingleMedia));
        strcpy(tmpsingleMedia.albumname,dirnode->albumname);
        strcpy(tmpsingleMedia.dirname,dirnode->dirname);
        tmpsingleMedia.dir = dirnode;
        strcpy(tmpsingleMedia.medianame,media);
        tmpsingleMedia.mediatype = dirnode->mediatype;
        Play(&tmpsingleMedia,time);
    }
}

int FindAndLoadAllDir(char *path)
{
    int ret = 0;
    /*这两个步骤不能换，以xml为优先，如果xml里面已经
    有了再增加真实目录的时候会直接返回成功*/
    
    /*首先按照顺序将xml描述的专辑存放到内存*/
    ret = AddRadioAlbumXml(path);
    //if (ret != 0)  /*注意:出错不能返回，必须要进行下一步*/
    //    return -1;

    /*将真实的音频目录放到内存*/
    ret = AddRealAudioDir(path);
    if (ret != 0)
        return -1;
   
    return 0;
}

int LoadAllAudio(int scandisk)
{
    int aididx = 0;
    SingleMedia *curaudio = NULL;
    DirNode *dirnode;
    int ret = 0;
    int i = 0;
    clearAllDirTable();
    ret = FindAndLoadAllDir(g_player_info.mountpoint);
    if (ret != 0)
    {
        return -1;
    }
    
    /*如果调用了扫描所有歌曲，则将本地歌单全部删除*/
    if (scandisk)
    {
        setAllDirInvalid();
    }

    /*首先准备获取配置文件中记录的目录*/
    dirnode = GetDirNodeByName(GetCurDirName());
    if ( NULL == dirnode)
    {
        /*目录不存在时，获取内存中记录的第一个真实目录*/
        log("no dir:%s g_player_info.dirarray.listidx=%d\n",GetCurDirName(),
                g_player_info.dirarray.listidx);
        /**/
        
        for (i=0;i<g_player_info.dirarray.listidx;i++)
        {
            dirnode = GetDirNodeByIdx(i,1);
            //log("dirnode->dirname=%s\n",dirnode->dirname)
            if( NULL == dirnode 
            || 0 == dirnode->audionum)
            {
                log("Get dir node failed\n");
                continue;
            }
            break;
        }
        /*如果所有目录都没有歌曲，直接返回*/
        if (i == g_player_info.dirarray.listidx)
        {
            log("not found valid music dir\n");
            /*找不到一个专辑*/
            setAllDirInvalid();
            return -1;
        }
    }
    else
    {
        /*如果存在需要播放的歌曲，首先播放*/
        LoadFirstPlay(dirnode,GetCurMediaName(),GetCurTime(),g_player_info.playauto);
        
        LoadDirFile(dirnode,scandisk);
        if (0 == dirnode->audionum)
        {
            /*如果当前正在播放的歌曲目录被删除了*/
            setAllDirInvalid();
            UpdateCurMediaInfo(NULL,0);
            
            return -1;
        }
        
    }
    
    if (dirnode == NULL)
    {
        log("not found valid music dir\n");
        return -1;
    }
    g_player_info.dirindex = GetDirIdx(dirnode);
    aididx = GetDirAudioIdxByName(dirnode,GetCurMediaName());
    if (aididx == -1)
    {
        log("no audio:%s in %s\n",GetCurMediaName(),dirnode->dirname);
        curaudio = GetDirFirstAudio(dirnode);
        if (NULL == curaudio)
        {
            log("Get audio failed\n");
            /*可能重新让所有目录失效*/
            //setAllDirInvalid();
            /*todo: 这里可能需要改变默认目录，使用下一个目录*/
            return -1;
        }
        //printf("curaudio %s  %s\n",curaudio->dirname,curaudio->medianame);
        //memset(&(g_player_info.curaudioinfo),0,sizeof(CurAudioInfo));
        
        //strcpy(g_player_info.curaudio,curaudio->medianame);
        dirnode->aididx = 0;
        UpdateCurMediaInfo(curaudio,0);
        //UpdataConfig();
        UpdateStatus();
    }
    else
    {
        /*设置当前的dir audio idx*/
        dirnode->aididx = aididx;
        
        curaudio = GetAudioByDirIdx(dirnode,aididx);
        //log("curaudio %s  %s\n",curaudio->dirname,curaudio->medianame);
        UpdateCurMediaInfo(curaudio,GetCurTime());
    }
    return ret;
}

#endif



static char *GetFileExtension(char *filename)
{
    char *tmp = NULL;
    tmp = filename;
    while (*tmp != '\0')
    {
        if (*tmp++ == '.')
            return tmp; 
    }
    return NULL;
}

static int CheckValidAudioFile(char *filename)
{
    int i;
    char *extension = GetFileExtension(filename);
    if (NULL == extension)
        return 0;
    
    //printf("extension=%s\n",extension);
    for (i = 0;i < sizeof(audiotype)/6;i++)
    {
        //printf("audiotype=%s\n",audiotype[i]);
        if (0 == strncmp(extension,audiotype[i],strlen(audiotype[i])))
        {
            return 1;
        }
    }
    
    return 0;
}

#ifdef SCAN_REAL_AUDIO
//char album[128][128] = {0};
static int CheckDirExist(char *dir)
{
    if (access(dir,F_OK) == 0)
        return 1;
    return 0;
}

static int AddRadioAlbumXml (const char *path)
{
    FILE *fd = NULL;
    char *buffer = NULL;
    int len;
    int i = 0;
    
    char filename[MAX_DIR_NAME]= {0};
    IXML_Node * node;
    
    char *tmpalbum = NULL;
    struct xmlelement *title_node;
    struct xmlelement *root;
    struct xmldoc *doc = NULL;
    /*open  文件radio_album.xml*/
    strcpy(filename,path);
    if (filename[strlen(filename) - 1] != '/')
            filename[strlen(filename)] = '/';
    strcat(filename,"radio_album.xml");
    
    if((fd=open(filename,O_RDONLY))<0){
        log("open file error");
        return -1; 
    }
    
    len=lseek(fd,0,SEEK_END);   
    buffer=mmap(NULL,len,PROT_READ,MAP_SHARED,fd,0);//读写得和open函数的标志相一致，否则会报错
    if(buffer==MAP_FAILED){
        log("mmap error");
        close(fd);
        return -1; 
    }
    
    doc = xmldoc_parsexml(buffer);
    if (doc == NULL)
    {
        log("xml parse error\n");
        munmap(buffer,len);
        close(fd);
        return -1;
    }
    
    root = find_element_in_doc(doc,"album_list");
    node = to_ielem(root);
    /*读取所有专辑列表，将目录保存*/
    node = ixmlNode_getFirstChild(node);
	for (/**/; node != NULL; node = ixmlNode_getNextSibling(node)) {
		if (strcmp(ixmlNode_getNodeName(node), "album") == 0) {
		    int ret = 0;

            if (i >= MAX_DIR_NUM)
                break;
            
            memset(filename,0,MAX_DIR_NAME);    
            strcpy(filename,path);
            
            if (filename[strlen(filename) - 1] != '/')
                filename[strlen(filename)] = '/';
			//return (struct xmlelement*) node;
            /*找到专辑*/
            title_node=find_element_in_element(node,"title");
            
            tmpalbum=get_node_value(title_node);
            
            //log("xml tmpalbum=%s\n",tmpalbum);
            
            //strcpy(album[i++],tmpalbum);
            strcat(filename,tmpalbum);
            
            if (CheckDirExist(filename))
            {
                //log("CheckDirExist tmpalbum=%s\n",tmpalbum);
                /*   增加/  */
                if (filename[strlen(filename) - 1] != '/')
                    filename[strlen(filename)] = '/';
                AddDirNullAlbum(filename,tmpalbum);
                i++;
            }
		}
	}
	xmldoc_free(doc);
    munmap(buffer,len);
    close(fd);
	
    return 0;
}



static int ScanAudioFile (const char *path,const char *album,int mediatype)
{
    DIR *dp;          //目录流
    struct dirent *entry;     //目录项信息
    struct stat statbuf;
    char tmppath[MAX_DIR_NAME] = {0};
    char tmpxmlname[MAX_DIR_NAME] = {0};
    //打开目录, 判断目录是否存在
    
    if ((dp = opendir (path)) == 0)
    {
        fprintf (stderr, "open dir failed::: %s\n ", path);
        perror("");
        return -1;
    }

    //读取目录信息
    while ((entry = readdir(dp)) != 0)
    {
        //忽略./..目录
        if (!strncmp (entry->d_name, ".",1) || !strncmp (entry->d_name, "..",2))
        {
            continue;
        }

        if (!strcmp (entry->d_name, "lost+found"))
        {
            continue;
        }

        //获取扫描到的文件的信息, 如果路径中没有'/', 则加'/', 加入strvec
        //不管是目录,还是文件,都将被加进去.

        //if (*(tmp_path.end () - 1) != '/')
        //    tmp_path += '/';        
        //printf("filename=%s\n",entry->d_name);
        //tmp_path += entry->d_name;
        //strvec.push_back (tmp_path);
        /*   增加/  */
        strcpy(tmppath,path);
        
        if (tmppath[strlen(tmppath) - 1] != '/')
            tmppath[strlen(tmppath)] = '/';
        //printf("%s\n",tmppath);

        //strcpy(tmpxmlname,tmppath);
        //strcat(tmpxmlname,"album_info.xml");
        
        /*如果存在album_info.xml，则添加*/
        //if (0 == access(tmpxmlname,F_OK))
        //{
        if (CheckValidAudioFile(entry->d_name) && album != NULL)
        {
            //log("audio=%s mediatype=%d\n",entry->d_name,mediatype);
            AddAudioFile(tmppath,album,entry->d_name,mediatype);
        }
        //}

        //如果是目录, 递归的扫描
        if (entry->d_type == 4)
        {
            strcat(tmppath,entry->d_name);
            /*   增加/  */
            if (tmppath[strlen(tmppath) - 1] != '/')
                tmppath[strlen(tmppath)] = '/';

            //printf("tmppath=%s\n",tmppath);
            strcpy(tmpxmlname,tmppath);
            strcat(tmpxmlname,"album_info.xml");
            if (0 == access(tmpxmlname,F_OK))
            {
                ScanAudioFile(tmppath,entry->d_name,GetMeidaType(tmppath));
            }
        }
        memset(tmppath,0,MAX_DIR_NAME);
    }
    closedir (dp);

    return 0;
}

#endif


/*



*/
static int Pause()
{
    begintoplay = 1;
	//NPT_String value;
	if (GetPlayStatus() == MPLAYER_PLAYING)
	{
		//pthread_mutex_lock(&mutex);
		//printf("OnPause\n");
		SetPlayStatus(MPLAYER_PAUSE);
		WriteCMDToMPlayer("pause");
		//pthread_mutex_unlock(&mutex);
	}
	else if (GetPlayStatus() == MPLAYER_PAUSE)
	{
        SetPlayStatus(MPLAYER_PLAYING);
		WriteCMDToMPlayer("pause");
	}
	return SUCCESS;
}

#ifdef PLAY_ADS
static void SendInfoToAd(SingleMedia *audio)
{
    if (audio->mediatype == MEDIA_AD)
    {
       server_xmit_play_info_api(g_adserver,audio->mediatype,
            audio->audioid,g_adaudio.plan_id,lastplayaudio.mediatype,lastplayaudio.audioid); 
       return;
    }
    
    if (strlen(lastplayaudio.audioid) == 0)
    {
        server_xmit_play_info_api(g_adserver,audio->mediatype,
            audio->audioid,0,1,"-1");
    }
    else
    {
        server_xmit_play_info_api(g_adserver,audio->mediatype,
            audio->audioid,0,lastplayaudio.mediatype,lastplayaudio.audioid);
    }
}
#endif
static int Play(SingleMedia *audio,int seconds)
{
	char cmd[512] = {0};
    char buf[512] = {0};
    char url[MAX_DIR_NAME] = {0};
    //char linkaudio[512] = {0};
    int ret = 0;
    char albumname[MAX_ALBUM_NAME] = {0};
#if 0    
	if (GetPlayStatus() == MPLAYER_PAUSE)
	{
		if (WriteCMDToMPlayer("pause") < 0) /*resend pause to restart mplayer*/
		{
    	    return -1;
		}
		SetPlayStatus(MPLAYER_PLAYING);
		return SUCCESS;
	}
#endif
    if (audio == NULL)
    {
        log("audio is NULL\n");
        return -1;
    }
    
    memset(url,0,MAX_DIR_NAME);
    //SetPlayStatus(MPLAYER_STOP);
    
	//printf("url=%s\n",url);
    //WriteCMDToMPlayer("stop");
    Stop();
    /*构建url*/
	strcpy(url,audio->dirname);
	strcat(url,audio->medianame);
    //log("url=%s\n",url);
    if (access(url,F_OK) != 0)
    {
        return SUCCESS;
    }
    
    /*
        TTS规则 :
    1）总是播放：就是只要切就播放；
    0）智能播放：只有在播放专辑时，并且用户主动发生切歌行为，再播放；
    2）不播放：顾名思义；
    */
    /*判断是否需要播放专辑名*/
    if ((g_player_info.ttsmode != DISABLE_TTS)
    && (g_player_info.switch_dir == 1))
    {
        //这个标志会被信号中断函数更新，如果在播放tts的时候更新了，则不继续播了
        stopttsflags = 0; 
        
        sprintf(albumname,"%s.",audio->albumname);
        log("tts abume:%s\n",albumname);
        playtts(albumname);
        //playtts(audio->dir->albumname);
        //log("stopttsflags:%d\n",stopttsflags);
        /*检测是否有按键，如果按键了，则直接返回*/
        if (stopttsflags == 1)
        {
            //SetPlayStatus(MPLAYER_STOP);
            return SUCCESS;
        }
    }
    log("url:%s %d \n",url,audio->mediatype);
    /*判断是否需要播放歌曲名*/
    if (audio->mediatype == MEDIA_AUDIO)
    {
    	if ((g_player_info.ttsmode == ALWAYS_TTS)
    	|| (g_player_info.ttsmode == SMART_TTS
        && g_player_info.manual == 1 
        && g_player_info.switch_dir != 1) )
        {
            //这个标志会被信号中断函数更新，如果在播放tts的时候更新了，则不继续播了
            stopttsflags = 0; 
            log("tts media:%s\n",audio->medianame);
            playtts(audio->medianame);
            /*检测是否有按键，如果按键了，则直接返回*/
            if (stopttsflags == 1)
            {
                //SetPlayStatus(MPLAYER_STOP);
                return SUCCESS;
            }
        }
    }
    //log("url:%s\n",url);
    
    /**************test*****************/
    //sprintf(linkaudio,"ln -sf %s  /tmp/nowplay",url);
    //system(linkaudio);
    //strcpy(url,"/tmp/nowplay");
    /*********************/
    
	sprintf(cmd,"loadfile \"%s\"",url);
	pthread_mutex_lock(&(g_player_info.mutex));
    /*here set play status in stop,avaid get position read*/
    //WriteCMDToMPlayer("stop");
    //SetPlayStatus(MPLAYER_STOP);

	//strcpy(g_player_info.curPlay.curtime,"0.000000");
	//strcpy(g_player_info.curPlay.totaltime,"0.000000");
	
	if (WriteCMDToMPlayer(cmd) < 0)
	{
	    pthread_mutex_unlock(&(g_player_info.mutex));
	    //SetPlayStatus(MPLAYER_STOP);
	    return -1;
	}

	if (WriteCMDToMPlayer(cmd) < 0)
	{
	    pthread_mutex_unlock(&(g_player_info.mutex));
	    //SetPlayStatus(MPLAYER_STOP);
	    return -1;
	}

	//SetMute();
	/*
	usleep(10000);
	WriteCMDToMPlayer("volume 0.1 1");
	usleep(10000);
	WriteCMDToMPlayer("volume 10.0 1");
	usleep(10000);
	WriteCMDToMPlayer("volume 50.0 1");
	usleep(10000);
	WriteCMDToMPlayer("volume 80.0 1");
	usleep(10000);
	WriteCMDToMPlayer("volume 0.0 1");
	*/
	/*wait for start to return*/
	while (ret = GetLine(buf,512,10000))
    {
		//log("%s",buf);
        if(strncmp(buf,"Starting playback",strlen("Starting playback")) == 0)
        {
            //printf("start play ret=%d\n",ret);
            break;
        }
        bzero(buf,512);
    }
    if (ret == 0)
    {
        //printf("Play    :   get buffer timeout\n");
        pthread_mutex_unlock(&(g_player_info.mutex));
        //SetPlayStatus(MPLAYER_STOP);
        return -1;
    }
    if (seconds > 0)
    {
        sprintf(cmd,"seek %d 2",seconds);
    	WriteCMDToMPlayer(cmd);
    }
	//sprintf(cmd,"volume %s.0 1",(char*)Volume);
    //WriteCMDToMPlayer(cmd);
    //WriteCMDToMPlayer("volume 0.0 1");
	//WriteCMDToMPlayer("volume 0.0 1");
	//usleep(10000);
	//WriteCMDToMPlayer("volume 0.1 1");
	//usleep(10000);
	//WriteCMDToMPlayer("volume 10.0 1");
	//usleep(10000);
	//WriteCMDToMPlayer("volume 100.0 1");
	//usleep(10000);
    SetPlayStatus(MPLAYER_PLAYING);
#ifdef PLAY_ADS
    SendInfoToAd(audio);
#endif    
    memcpy(&lastplayaudio,audio,sizeof(SingleMedia));
    
    pthread_mutex_unlock(&(g_player_info.mutex));
    
    return SUCCESS;

}

static int CheckStorage()
{
    FILE *pp = NULL;
    char buffer[2] = {0};
    pp = popen("/sbin/CheckMountInfo.sh","r");
    if (pp == NULL)
    {
        return -1;
    }
    fgets(buffer,64,pp);
    pclose(pp);
    if (strncmp(buffer,"0",1) == 0)
        return -1;

    return 0;
}

static int CheckAudioFile(char *url)
{
    
    return 0;
}

void setPlayMode(int mode)
{
    g_player_info.playmode = mode;
    UpdateConfig();
    //printf("g_player_info.playmode=%d\n",g_player_info.playmode);
}

int getPlayMode(void)
{
    //printf("g_player_info.playmode=%d\n",g_player_info.playmode);
    return g_player_info.playmode;
}

void setPlayAutoMode(int pauto)
{
    g_player_info.playauto = pauto;
    UpdateConfig();
}

int getPlayAutoMode(void)
{
    return  g_player_info.playauto;
}

int getTTSMode(void)
{
    return  g_player_info.ttsmode;
}

void setTTSMode(int ttsmode)
{
    g_player_info.ttsmode = ttsmode;
    UpdateConfig();
}


static int ReadStatus()
{
    FILE *fd = NULL;
    char buffer[512] = {0};
    char seconds[8] = {0};
    char curdir[MAX_DIR_NAME] = {0};
    char curaudio[MAX_MEDIA_NAME] = {0};
    fd = fopen(MUSIC_STATUS_FILE,"r+");
    if (fd < 0)
    {
        log("open music.conf error\n");
        return -1;
    }

    while (0 != fgets(buffer,512,fd))
    {
        if (0 == strncmp("CUR_DIR",buffer,7))
            sscanf(buffer,"CUR_DIR=%[^\n]",curdir);
        else if (0 == strncmp("CUR_AUDIO",buffer,9))
            sscanf(buffer,"CUR_AUDIO=%[^\n]",curaudio);
        else if (0 == strncmp("PLAY_SECONDS",buffer,12))
            sscanf(buffer,"PLAY_SECONDS=%[^\n]",seconds);
        else
            continue;
    }
   // printf("g_player_info.mountpoint=%s\n",g_player_info.mountpoint);
    //printf("g_player_info.curdir=%s\n",g_player_info.curdir);
    
    SetCurMeidaCurDir(curdir);
    SetCurMediaCurAudio(curaudio);
    SetCurMediaCurTime(atoi(seconds));
    
    fclose(fd);
    return 0;
}

static int UpdateStatus()
{
    FILE *fd = NULL;
    fd = fopen(MUSIC_STATUS_FILE,"w+");
    if (fd < 0)
    {
        log("open music.status error\n");
        return -1;
    }

    fprintf(fd,"CUR_DIR=%s\n",GetCurDirName());
    fprintf(fd,"CUR_AUDIO=%s\n",GetCurMediaName());
    fprintf(fd,"PLAY_SECONDS=%d\n",GetCurTime());
    fflush(fd);
    fclose(fd);
    
    return 0;
}

void UpdateCtlStatus(void)
{
    FILE *fp = NULL;
    if (access(CONTRL_PORT"/playmode",F_OK) != 0)
    {
        fp = fopen(CONTRL_PORT"/playmode","w+");
        if (NULL == fp)
            return; 
        fprintf(fp,"%d\n",g_player_info.playmode);
        fflush(fp);
        fclose(fp);
    }

    if (access(CONTRL_PORT"/firstplay",F_OK) != 0)
    {
        fp = fopen(CONTRL_PORT"/firstplay","w+");
        if (NULL == fp)
            return; 
        fprintf(fp,"%d\n",g_player_info.playauto);
        fflush(fp);
        fclose(fp);
    }

    if (access(CONTRL_PORT"/ttsmode",F_OK) != 0)
    {
        fp = fopen(CONTRL_PORT"/ttsmode","w+");
        if (NULL == fp)
            return; 
        fprintf(fp,"%d\n",g_player_info.ttsmode);
        fflush(fp);
        fclose(fp);
    }
    
    if (access(CONTRL_PORT"/syncbeat",F_OK) != 0)
    {
        fp = fopen(CONTRL_PORT"/syncbeat","w+");
        if (NULL == fp)
            return; 
        fprintf(fp,"%d\n",AUDIO_SYNC_NO);
        fflush(fp);
        fclose(fp);
    }

    return;
}

static int ReadConfig()
{
    FILE *fd = NULL;
    char buffer[512] = {0};
    char playmode[8] = {0};
    char playauto[3] = {0};
    char ttsmode[3] = {0};
    //char syncstatus[3] = {0};
reopenconfig:    
    fd = fopen(g_player_info.cfile,"r+");
    if (fd < 0)
    {
        log("open music.conf error\n");
        return -1;
    }

    while (0 != fgets(buffer,512,fd))
    {
        if (0 == strncmp("MOUNT_DIR",buffer,9))
            sscanf(buffer,"MOUNT_DIR=%[^\n]",g_player_info.mountpoint);
        else if (0 == strncmp("PLAY_MODE",buffer,9))
            sscanf(buffer,"PLAY_MODE=%[^\n]",playmode);
        else if (0 == strncmp("PLAY_AUTO",buffer,9))
            sscanf(buffer,"PLAY_AUTO=%[^\n]",playauto);
        else if (0 == strncmp("TTS_MODE",buffer,8))
            sscanf(buffer,"TTS_MODE=%[^\n]",ttsmode);
        //else if (0 == strncmp("SYNC_STATUS",buffer,11))
        //    sscanf(buffer,"SYNC_STATUS=%[^\n]",syncstatus);
        else
            continue;
    }
    /*避免配置文件被破坏*/
    if (strlen(g_player_info.mountpoint)== 0)
    {
        system("cp /etc/music.conf "MUSIC_CONFIG_FILE);
        fclose(fd);
        goto reopenconfig;
    }
   // printf("g_player_info.mountpoint=%s\n",g_player_info.mountpoint);
    //printf("g_player_info.curdir=%s\n",g_player_info.curdir);
    
    g_player_info.playmode = atoi(playmode);
    g_player_info.playauto = atoi(playauto);
    g_player_info.ttsmode = atoi(ttsmode);
    //g_player_info.syncstatus = atoi(syncstatus);
    log("g_player_info.mountpoint=%s\n",g_player_info.mountpoint);
    log("g_player_info.playmode=%d\n",g_player_info.playmode);
    log("g_player_info.ttsmode=%d\n",g_player_info.ttsmode);
    //log("g_player_info.syncstatus=%d\n",g_player_info.syncstatus);

    fclose(fd);
    
    UpdateCtlStatus();
    return 0;
}

int UpdateConfig()
{
    FILE *fd = NULL;
    fd = fopen(g_player_info.cfile,"w+");
    if (fd < 0)
    {
        log("open music.conf error\n");
        return -1;
    }

    fprintf(fd,"MOUNT_DIR=%s\n",g_player_info.mountpoint);
    fprintf(fd,"PLAY_MODE=%d\n",g_player_info.playmode);
    fprintf(fd,"PLAY_AUTO=%d\n",g_player_info.playauto);
    fprintf(fd,"TTS_MODE=%d\n",g_player_info.ttsmode);
    fprintf(fd,"SYNC_STATUS=%d\n",g_player_info.syncstatus);
    fflush(fd);
    fclose(fd);
    
    return 0;
}

static void clearAllDirTable(void)
{
    
    g_player_info.dirarray.listidx = 0;
    //g_player_info.audioarray.listidx = 0;
    memset(g_player_info.dirarray.list,0,MAX_DIR_NUM*sizeof(DirNode));
    //memset(g_player_info.dirarray.list,0,MAX_AUDIO_NUM*sizeof(SingleMedia));
}

/*检测是否有挂起的按键消息*/
static int HasButtonPadding()
{
    if (g_player_info.button.nextaudiopad
    || g_player_info.button.prevaudiopad
    || g_player_info.button.nextdirpad
    || g_player_info.button.prevdirpad)
    {
        return 1;
    }

    return 0;
}

static void ClearButtonPadding()
{
    g_player_info.button.nextaudiopad = 0;
    g_player_info.button.prevaudiopad = 0;
    g_player_info.button.nextdirpad = 0;
    g_player_info.button.prevdirpad = 0;
    
}

static int ButtonSwitch()
{
    int i = 0;
    int diridx;
    static SingleMedia *nextaudio;
    DirNode *dirnode = GetDirNodeByIdx(g_player_info.dirindex,0);

    /*如果收到广告消息，则播放广告，同时关闭按键功能*/
    if (g_adaudio.enable == 1)
    {
        g_player_info.button.disabled = 1;
        UpdateCurMediaInfo(&(g_adaudio.media),0);
        ClearButtonPadding();
        return 0;
    }
    if (dirnode == NULL)
    {
        return 0;
    }
    for (i = 0;i < g_player_info.button.nextaudiopad;i++)
    {
        dirnode->seqorder = 0;
        g_player_info.manual = 1;
	    nextaudio = FindNextAudioInDir(dirnode);
    }

    for (i = 0;i < g_player_info.button.prevaudiopad;i++)
    {
        dirnode->seqorder = 1;
        g_player_info.manual = 1;
	    nextaudio = FindNextAudioInDir(dirnode);
    }
    
    for (i = 0;i < g_player_info.button.nextdirpad;i++)
    {
        ClearRandomList();
	
    	diridx = NextDirIdx(dirnode);
        if (diridx == -1)
        {
            diridx = 0;
        }
        
        g_player_info.dirindex = diridx;
        
        dirnode = GetDirNodeByIdx(diridx,1);
        if (dirnode == NULL)
        {
            return 0;
        }
        dirnode->aididx = 0;
        nextaudio = GetAudioByDirIdx(dirnode,dirnode->aididx);
        
        /*audio info*/
        UpdateCurMediaInfo(nextaudio,0);
        g_player_info.switch_dir = 1;
        //UpdateStatus();
    }

    for (i = 0;i < g_player_info.button.prevdirpad;i++)
    {
        ClearRandomList();
    	g_player_info.manual = 1;
        diridx = PrevDirIdx(dirnode);
        if (diridx == -1)
        {
            diridx = 0;
        }
        g_player_info.dirindex = diridx;
        
        dirnode = GetDirNodeByIdx(diridx,1);
        if (dirnode == NULL)
        {
            return 0;
        }
        dirnode->aididx = 0;
        nextaudio = GetAudioByDirIdx(dirnode,dirnode->aididx);
        
        /*audio info*/
        //printf("nextaudio:%s\n",nextaudio->medianame);
        UpdateCurMediaInfo(nextaudio,0);
        g_player_info.switch_dir = 1;
        
    }
    //log("ButtonSwitch:%s %d\n",nextaudio->medianame,nextaudio->mediatype);
    UpdateStatus();
    
    ClearButtonPadding();
    return 0;
}

static int MainLoop()
{
    int ret;
    int aididx;
    DirNode *dirnode;
    int firstplay = 1;
    
    SingleMedia *curaudio = NULL;

    /*检测外置存储状态*/
    if (CheckStorage())
    {
        log("storage not ready\n");
        return -1;
    }
    /*read config from music.conf*/
    ReadConfig();
    ReadStatus();
    while (1)
    {
        /*检测是否需要重新扫描*/
        ret = LoadAllAudio(0);
        if (ret != 0)
        {
            usleep(5000000);
            continue;
            //exit(0);
        }
        else
        {
            break;
        }
    }
    log("scanf end\n");
    signal(SIG_MY_MSG1,&NextMediaSigHandler);
    signal(SIG_MY_MSG2,&PrevMediaSigHsandler);
    signal(SIG_MY_MSG3,&NextDirSigHandler);
    signal(SIG_MY_MSG4,&PrevDirSigHandler);
    signal(SIG_MY_MSG4,&PrevDirSigHandler);
    signal(SIG_PAUSE,&PauseSigHandler);
    signal(SIG_SETTING,&SettingSigHandler);
    
//#define TEST
#ifdef TEST
    DirNode *dir;
    SingleMedia *audio;
    int i;
    int j;
    log("dir tree:\n");
    for (i=0;i <g_player_info.dirarray.listidx;i++)
    {
        
        dir = &(g_player_info.dirarray.list[i * sizeof(DirNode)]);
        log("%s \n",dir->dirname);
        for (j=0;j < dir->audionum;j++)
        {
            audio = GetAudioByDirIdx(dir,j);
            log("%s\n",audio->medianame);
        }
    }
    
    log("audio list:\n");
    for (i=0;i <g_player_info.audioarray.listidx;i++)
    {
        audio = &(g_player_info.audioarray.list[i * sizeof(SingleMedia)]);
        log("%s \n",audio->medianame);
    }
#endif

    /*check user*/
    //printf("curaudio=%s | %s",curaudio->dirname,curaudio->medianame);
    
    dirnode = GetDirNodeByIdx(g_player_info.dirindex,1);
    curaudio = GetAudioByDirIdx(dirnode,dirnode->aididx);
    begintoplay = g_player_info.playauto;
    /*如果当前已经播放，则设置firstplay为0*/
    if (MPLAYER_PLAYING == GetPlayStatus())
    {
        firstplay = 0;
    }
    //system("/usr/sbin/awctl ledon");
    SyncLedCtl(0);
    /**/
    while(1)
    {
        /*收到同步结束信号，则重新扫描*/
        if (g_player_info.syncstatus == AUDIO_SYNC_END)
        {
#ifdef PLAY_ADS
            server_xmit_sync_info_api(g_adserver, APP_SYNC_END);
#endif
            SyncLedCtl(0);
            system("apachectl stop");
            usleep(500000);
            system("apachectl -f /usr/apache/httpd.conf &");
            usleep(500000);
            log("begin reload file\n");
            
            #if 1
            /*先删除之前的目录*/
            setAllDirInvalid();
            ret = LoadAllAudio(1);
            if (ret !=  0)
            {
                usleep(3000000);
                continue;
            }
            #endif
            system("sync");
            g_player_info.syncstatus = AUDIO_SYNC_NO;
            
            UpdateConfig();
            
            //RemoveAllDirWatch();
            log("begin reload end\n");
        }
        
        if (HasButtonPadding())
        {
            Stop();
            #if 0
            while (sleepforbutton)
            {
                /*此变量会被按键处理函数更新，如果检测到按键则继续
                             等0.1s*/
                sleepforbutton = 0;
                usleep(100000);
            }
            #endif
        }
        
        /*  查找播放文件，开始播放*/
        if (MPLAYER_STOP == GetPlayStatus() && begintoplay == 1)
        {
            if ((firstplay == 0)  
            && !HasButtonPadding())
            {
                dirnode = GetDirNodeByIdx(g_player_info.dirindex,1);
                if (dirnode == NULL)
                {
                    continue;
                }
                /*检测下一首要播放的歌曲*/
                /*根据相应的策略，判断是否要播放广告*/
                curaudio = FindNextDirAudio(dirnode);
                if (curaudio == NULL)
                {
                    //printf("no audio player:stop\n");
                }
                else
                {
                    log("curaudio=%s | %s | %s | %d\n",curaudio->dirname,curaudio->albumname,
                    curaudio->medianame,curaudio->mediatype);
                    //log("curaudio=%s || %s | %d\n",curaudio->dirname,
                    //    curaudio->medianame,curaudio->mediatype);
                }
            }
            else if (HasButtonPadding())
            {
                ButtonSwitch();
                /*找到的歌已经在播放了，就暂时不播了*/
                if (MPLAYER_PLAYING == GetPlayStatus())
                {
                    continue;
                }
            }
            
            if (GetCurSingleMedia() != NULL)
            {
                if (GetCurSingleMedia()->mediatype != MEDIA_AUDIO)
                {
                    /*播放当前url,播放为异步*/
                    Play(GetCurSingleMedia(),0);
                }
                else
                {
                    /*如果tts是ALWAYS,第一次启动时需要播放*/
                    if ( (firstplay == 1) && (g_player_info.ttsmode == ALWAYS_TTS) )
                    {
                        //log("first player\n");
                        g_player_info.switch_dir = 1;
                    }
                    //GetCurSingleMedia();
                    /*对于音频文件，第一次启动从上次播放的时间开始播放*/
                    Play(GetCurSingleMedia(),GetCurTime());
                }
                
                if (MPLAYER_PLAYING== GetPlayStatus())
                {
                    g_player_info.manual = 0;
                    g_player_info.switch_dir = 0;
                }
                /*广告已经使能，下次播放关闭*/
                if (g_adaudio.enable == 1)
                {
                    //g_player_info.button.disabled = 0;
                    g_adaudio.enable = 0;
                }
                
                firstplay = 0;
            }
        }
        
        usleep(300000);

    }

    return 0;
}


int Previous()
{
	log("OnPrevious\n");
	
	return SUCCESS;
}

int Stop()
{
	//return Stop();
	//pthread_mutex_lock(&mutex);
	//printf("OnStop\n");
	//WriteCMDToMPlayer("volume 10.0 1");
	//usleep(10000);
	//WriteCMDToMPlayer("volume 1.0 1");
	//usleep(10000);
	//WriteCMDToMPlayer("volume 0.1 1");
	//usleep(20000);
	//WriteCMDToMPlayer("volume 0.0 1");
	//usleep(20000);
	SetMute();
	usleep(10000);
	SetPlayStatus(MPLAYER_STOP);
	//WriteCMDToMPlayer("mute 1");
	
	WriteCMDToMPlayer("stop");
	//pthread_mutex_unlock(&mutex);
	
	return SUCCESS;
}


int SetMute()
{
	
	//if (GetPlayStatus() != MPLAYER_PLAYING)
	//    return SUCCESS;
	
	//service->GetStateVariableValue("Mute",mvalue);
	//printf("AVTransport mute %s\n",(char*)mvalue);
	
	WriteCMDToMPlayer("mute 1");
	//pthread_mutex_unlock(&mutex);
	
	return SUCCESS;
}
int notifyTTS()
{
    stopttsflags = 1;
    stopnowtts();
    return 0;

}

void NextMediaSigHandler(int signumber)
{
    if (g_player_info.button.disabled)
        return;
    g_player_info.button.nextaudiopad++;
    notifyTTS();
    sleepforbutton = 1;
	return ;
	
}

void PrevMediaSigHsandler(int signumber)
{
    if (g_player_info.button.disabled)
        return;
    g_player_info.button.prevaudiopad++;
    notifyTTS();
    sleepforbutton = 1;
	return ;
	
}

void NextDirSigHandler(int signumber)
{
    if (g_player_info.button.disabled)
        return;
    g_player_info.button.nextdirpad++;
    notifyTTS();
    sleepforbutton = 1;
}

void PrevDirSigHandler(int signumber)
{
    if (g_player_info.button.disabled)
        return;
    g_player_info.button.prevdirpad++;
    notifyTTS();
    sleepforbutton = 1;
    
}

void PauseSigHandler(int signumber)
{
    if (g_player_info.button.disabled)
        return;
    Pause();
}

void SettingSigHandler(int signumber)
{
    FILE *fp = NULL;
    int ttsmode = 0;
    int playmode = 0;
    int firstplay = 0;
    int syncbeat = 0;
    
    char tmpbuffer[32] = {0};
    
    fp = fopen(CONTRL_PORT"/ttsmode","r");
    if (NULL == fp)
        return -1; 
    fgets(tmpbuffer,32,fp);
    sscanf(tmpbuffer,"%d",&ttsmode);
    fclose(fp);
    setTTSMode(ttsmode);
    memset(tmpbuffer,0,32);
    
    fp = fopen(CONTRL_PORT"/playmode","r");
    if (NULL == fp)
        return -1; 
    fgets(tmpbuffer,32,fp);
    sscanf(tmpbuffer,"%d",&playmode);
    fclose(fp);
    setPlayMode(playmode);
    memset(tmpbuffer,0,32);
    
    fp = fopen(CONTRL_PORT"/firstplay","r");
    if (NULL == fp)
        return -1; 
    fgets(tmpbuffer,32,fp);
    sscanf(tmpbuffer,"%d",&firstplay);
    fclose(fp);
    setPlayAutoMode(firstplay);
    memset(tmpbuffer,0,32);
    
    fp = fopen(CONTRL_PORT"/syncbeat","r");
    if (NULL == fp)
        return -1; 
    fgets(tmpbuffer,32,fp);
    sscanf(tmpbuffer,"%d",&syncbeat);
    fclose(fp);
    beginSync(syncbeat);
    return ;
}


int main(int argc,char **argv)
{
    int ret;
    //char *cfile = (char *)param;
    pid_t pid;
    char *cfile;
	gpipe = -1;
    gmpipe = -1;
    signal(SIG_MY_MSG1,SIG_IGN);
    signal(SIG_MY_MSG2,SIG_IGN);
    signal(SIG_MY_MSG3,SIG_IGN);
    signal(SIG_MY_MSG4,SIG_IGN);
    signal(SIG_MY_MSG4,SIG_IGN);
    signal(SIG_PAUSE,SIG_IGN);
    signal(SIG_SETTING,SIG_IGN);
    
    if (argc == 2)
        cfile = argv[1];
    else
        cfile = "/etc/music.conf";
    
    OPEN_LOG;
    memset(&g_player_info,0,sizeof(g_player_info));
    memset(&g_adaudio,0,sizeof(g_adaudio));
    memset(&lastplayaudio,0,sizeof(lastplayaudio));
    
    g_player_info.playmode = PLAY_MODE_SEQUENCE;
    //g_player_info.curaudioinfo.media
    g_player_info.disablectl = 0;
    g_player_info.playstatus = MPLAYER_STOP;
    g_player_info.nowplaytimes = 0;

    
    /*分配dirlist*/
    g_player_info.dirarray.list = malloc(MAX_DIR_NUM * sizeof(DirNode));
    if (NULL == g_player_info.dirarray.list)
    {
        log("malloc dir list error\n");
        return -1;
    }

   // printf("cfile=%p\n",cfile);
    //printf(" dirarray=%p\n", &(g_player_info.dirarray));
    g_player_info.cfile = (char *)cfile;
    log("g_player_info.cfile=%s\n",g_player_info.cfile);

    //g_player_info.dirwtd = inotify_init1(IN_NONBLOCK);
    
	if (pipe2(FdPipe,O_NONBLOCK)<0 ) 
	//if (pipe(fd_pipe)<0 ) 
	{
		log("pipe error\n");  
		return -1;
	}
	pid=fork();
	if(pid<0)  
	{
		log("fork"); 
		CLOSE_LOG;
		return -1;
	}
	
	if(pid==0)              //子进程播放mplayer  
	{
		close(FdPipe[0]);
		dup2(FdPipe[1],1); 
		dup2(FdPipe[1],2); 
		/*
		execlp("mplayer","mplayer","-quiet","-slave",
		            "-cache","4096","-cache-min","20","-cache-seek-min","80","-input","file=/tmp/mfifo","-idle","-ao","alsa",
		            "-codecs-file","/etc/codecs.conf","-format","s16ne","-srate","44100","NULL");
		*/
		log("Open mplayer");
        
		execlp("mplayer","mplayer","-quiet","-slave",
		            "-cache","1024","-cache-min","20","-cache-seek-min","80",
		            "-input","file=/tmp/mfifo","-idle","-ao","oss",
		            "-codecs-file","/etc/codecs.conf","-af","channels=2",
		            "-af","resample=44100","-format","s16ne",NULL);
        /*
		execlp("mplayer","mplayer","-quiet","-slave",
		            "-cache","1024","-cache-min","20","-cache-seek-min","80",
		            "-input","file=/tmp/mfifo","-idle","-ao","oss",
		            "-codecs-file","/etc/codecs.conf","-af","channels=2",
		            "-format","s16ne",NULL);*/
		            /*
		execlp("mplayer","mplayer","-quiet","-slave",
		            "-cache","1024","-cache-min","20","-cache-seek-min","80",
		            "-input","file=/tmp/mfifo","-idle","-ao","oss",
		            "-codecs-file","/etc/codecs.conf","-af","channels=2",
		            "-format","s16ne","-af","volume=15:0",NULL);*/
		//perror("");
		log("open error %s",strerror(errno));
	}
	else
	{
	    //exit(0);
		close(FdPipe[1]);
		gpipe = FdPipe[0];
		gmpipe = open("/tmp/mfifo",O_WRONLY);
		if (gmpipe == -1)
		{
			log("Open mfifo failed");

			gpipe = -1;
			CLOSE_LOG;
			return -1;
		}
		
		ret = pthread_create(&PosThread,NULL,UpdateTrackTimeThread,NULL);
		if (0 != ret)
		{
			log("Create wait_callback failed");
			CLOSE_LOG;
			return -1;	
		}
		
#ifdef PLAY_ADS
        ret = pthread_create(&AdThread,NULL,adserver_init,NULL);
		if (0 != ret)
		{
			log("Create ad server thread callback failed");
			CLOSE_LOG;
			return -1;
		}
		usleep(200000);
#endif
		pthread_mutex_init(&g_player_info.mutex,NULL);
		//sleep(1);
		//WriteCMDToMPlayer("loadfile NULL");
		//SetPlayStatus(MPLAYER_STOP);
	}
	
    ttsinit();
    MainLoop();	
    ttsdeinit();
    CLOSE_LOG;
    //mplayer quiet -slave  -idle
    return SUCCESS;
}



