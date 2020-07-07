#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#define K 1024

typedef struct
{
	char fileName[16]; //文件或目录名称
	int type; //文件类型，1为文本文件，2为目录
	int parent; //父目录索引
	int fileSize;  //文件大小，单位为一个盘块
	int bSize; //文件大小，单位为字节
	int wr; //写指针
	int index[4];  //盘块号索引，最多四个盘块，4KB
} iNode;

typedef struct
{
	char fileName[28]; //文件名
	int index; //对应文件的iNode索引
} dirStruct; //目录的存储单位

typedef struct
{
	int isUsed; //判断该结构体是否已被使用，1为已被使用，0为未使用
	char fileName[16]; //文件名
	sem_t rSem,wSem; //读写信号量
	int readers; //读文件进程数
} fileSem;

void* disk;
fileSem* fs;
int fsSize=64;
sem_t wrSem,stSem;
int wrs=0;
pthread_mutex_t lock1 = PTHREAD_MUTEX_INITIALIZER,lock2=PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t rwlock1,rwlock2;

void init()
{
	disk=malloc(100*K*K+4*3); //分配磁盘空间
	memset(disk,0,20*K*K+4*3);  //全部初始化为0

	int* guideDisk=(int*)disk;  //前三个int为引导块，导入信息
	*guideDisk=1;  //盘块大小，单位为KB，即盘块大小为1KB
	*(guideDisk+1)=15;  //iNode位示图大小，单位为KB
	*(guideDisk+2)=15;  //盘块位示图大小，单位为KB

	sem_init(&wrSem,0,1);
	sem_init(&stSem,0,1);
	pthread_rwlock_init(&rwlock1, NULL);
	pthread_rwlock_init(&rwlock2, NULL);

	fs=(fileSem*)malloc(sizeof(fileSem)*fsSize); //初始开辟64个文件的空间，后续可拓展
	memset(fs,0,sizeof(fileSem)*fsSize);
}

int pSize,inbSize,pbSize; //盘块大小，iNode图大小，盘块图大小
char* iNodeBP; //指向iNode位示图的指针
char* pBP; //指向盘块图的指针
iNode* iNodep; //指向iNode的指针
char* pp; //指向盘块的指针
iNode* rdir; //指向根目录的指针
char cdir[16]; //当前目录名称
char dirSource[1024]; //目录路径，最多63层
int cdirIn; //当前目录索引

void appendFs() //拓展fs
{
	pthread_rwlock_wrlock(&rwlock2);
	int orfs=fsSize;
	fsSize*=2;
	fileSem* te=(fileSem*)malloc(sizeof(fileSem)*fsSize);
	memset(te,0,sizeof(fileSem)*fsSize);
	for(int i=0;i<orfs;i++)
		te[i]=fs[i];
	free(fs);
	fs=te;
	pthread_rwlock_unlock(&rwlock2);
}

void deleteFs() //缩小fs
{
	pthread_rwlock_wrlock(&rwlock2);
	int orfs=fsSize;
	int sum=0;
	for(int i=0;i<fsSize;i++)
		if((fs+i)->isUsed)
			sum++;
	if(fsSize>=128 && sum<fsSize/2)
	{
		fsSize/=2;
		fileSem* te=(fileSem*)malloc(sizeof(fileSem)*fsSize);
		memset(te,0,sizeof(fileSem)*fsSize);
		int k=0;
		for(int i=0;i<orfs;i++)
		{
			if((fs+i)->isUsed)
			{
				memcpy(te+k,fs+i,sizeof(fileSem));
				k++;
			}
		}
		fs=te;
	}
	pthread_rwlock_unlock(&rwlock2);
}

int getDiskIn() //获取空闲盘块索引
{
	pthread_mutex_lock(&lock1);
        for(int i=0;i<pbSize;i++)
        {
                if(!(*(pBP+i)))
                {
			*(pBP+i)=1;
			pthread_mutex_unlock(&lock1);
                        return i;
                }
        }
	pthread_mutex_unlock(&lock1);
        return -1;
}

void initDir(int ind)
{
        int buf[K/4];

        for(int i=0;i<K/4;i++)
                buf[i]=-1;
        memcpy(pp+ind*K,buf,K);
}


void load()
{
	int* guideDisk=(int*)disk;
	pSize=*guideDisk;
	inbSize=*(guideDisk+1);
	pbSize=*(guideDisk+2);

	iNodeBP=(char*)(guideDisk+3);
	pBP=(char*)(iNodeBP+15*K);
	iNodep=(iNode*)(pBP+15*K);
	pp=(char*)(iNodep+15*K);

	int pIndex=getDiskIn();
	*(pBP+pIndex)=1;
	initDir(pIndex);
	rdir=iNodep;
	rdir->fileSize=1;
	rdir->type=2;
	rdir->bSize=0;
	rdir->parent=0;
	rdir->wr=0;
	rdir->index[0]=pIndex;
	*iNodeBP=1;
	strcpy(rdir->fileName,"root");
	strcpy(cdir,"root");
	strcpy(dirSource,"root/");
	cdirIn=0;
}

int getiNodeIn() //获取空闲的inNode节点索引
{
	pthread_mutex_lock(&lock2);
        for(int i=0;i<inbSize*K;i++)
        {
                if(!(*(iNodeBP+i)))
                {
			*(iNodeBP+i)=1;
			pthread_mutex_unlock(&lock2);
                        return i;
                }
        }
	pthread_mutex_unlock(&lock2);
	return -1;
}

int getFileIndex(char* fileName)
{
        if(!fileName || strlen(fileName)>15)
        {
                printf("文件名格式错误\n");
                return 0;
        }
	iNode* dir=iNodep+cdirIn;
        int nums=dir->fileSize;
        dirStruct buf[32];
        for(int i=0;i<nums;i++)
        {
                memcpy(buf,pp+(dir->index[i])*K,32*32);
                for(int j=0;j<32;j++)
                {
                        if(buf[j].index!=-1 && !strcmp(fileName,buf[j].fileName))
                        {
                                return buf[j].index;
                        }
                }
        }
        //printf("文件不存在\n");
        return -1;
}

int changeFileName(char* fileName1,char* fileName2)
{
//	printf("???\n");
//	pthread_mutex_lock(&lock2);
	if(!fileName1 || strlen(fileName1)>15 || !fileName2 || strlen(fileName2)>15)
        {
                printf("文件名格式错误\n");
                return 0;
        }
	int te=getFileIndex(fileName2);
	if(te!=-1)
	{
		printf("文件名重复\n");
		return 0;
	}
        iNode* dir=iNodep+cdirIn;
        int nums=dir->fileSize;
        dirStruct buf[32];
        for(int i=0;i<nums;i++)
        {
                memcpy(buf,pp+(dir->index[i])*K,32*32);
                for(int j=0;j<32;j++)
                {
                        if(buf[j].index!=-1 && !strcmp(fileName1,buf[j].fileName))
                        {
                                strcpy(buf[j].fileName,fileName2);
				int ind=buf[j].index;
				strcpy((iNodep+ind)->fileName,fileName2);
				memcpy(pp+(dir->index[i])*K,buf,32*32);
				return 1;
                        }
                }
        }
        printf("文件不存在\n");
//	pthread_mutex_unlock(&lock2);
        return 0;
}

int addDir(iNode* dir,char* fileName,int index)
{
	int nums=dir->fileSize;
	dirStruct buf[32];
	for(int i=0;i<nums;i++)
	{
		memcpy(buf,pp+(dir->index[i])*K,32*32);
		for(int j=0;j<32;j++)
		{
			if(buf[j].index==-1)
			{
				strcpy(buf[j].fileName,fileName);
			        buf[j].index=index;
				memcpy(pp+(dir->index[i])*K,buf,32*32);
				return 1;
			}
		}
	}
	if(nums<4)
	{
		int newIn=getDiskIn();
		initDir(newIn);
		memcpy(buf,pp+newIn*K,32*32);
		strcpy(buf[0].fileName,fileName);
                buf[0].index=index;
		memcpy(pp+newIn*K,buf,32*32);
		return 1;
	}
	printf("目录已满\n");
	return 0;
}

int changeDir(char* dirName)
{
	if(!strcmp(dirName,".."))
	{

		iNode* dir=(iNodep+cdirIn);
		cdirIn=dir->parent;
		strcpy(cdir,(iNodep+dir->parent)->fileName);
		for(int i=strlen(dirSource)-2;i>=0;i--)
			if(dirSource[i]=='/')
			{
				dirSource[i+1]=0;
				break;
			}
		return 1;
	}

	int index=getFileIndex(dirName);

	if(index==-1)
		return 0;
	if((iNodep+index)->type!=2)
	{
		printf("不是一个目录\n");
		return 0;
	}

	cdirIn=index;
	strcpy(cdir,dirName);
	strcat(dirSource,cdir);
	strcat(dirSource,"/");
	return 1;
}

void ls1()
{
        dirStruct buf[32];
	iNode* dir=(iNodep+cdirIn);
	int nums=dir->fileSize;
        for(int i=0;i<nums;i++)
        {
                memcpy(buf,pp+(dir->index[i])*K,32*32);
                for(int j=0;j<32;j++)
                {
			//printf("%d ",buf[j].index);
                        if(buf[j].index!=-1)
                        {
                                printf("fileName: %s  type: %d\n",buf[j].fileName,(iNodep+buf[j].index)->type);
                        }
                }
        }
}

int removeDir(iNode* dir,char* fileName)
{
	int nums=dir->fileSize;
	dirStruct buf[32];
        for(int i=0;i<nums;i++)
        {
		memcpy(buf,pp+(dir->index[i])*K,32*32);
                for(int j=0;j<32;j++)
                {
                        if(buf[j].index!=-1 && !strcmp(buf[j].fileName,fileName))
                        {
				int ind=buf[j].index;
                                buf[j].index=-1;
                                memcpy(pp+(dir->index[i])*K,buf,32*32);
                                return ind;
                        }
                }
        }
//	printf("文件不存在\n");
	return -1;
}

int open2(char* fileName,int type) //创建文件
{
	if(!fileName || strlen(fileName)>15)
	{
		printf("文件名格式错误！\n");
		return 0;
	}
	dirStruct buf[32];
	int nIndex=getiNodeIn();
	if(nIndex==-1)
	{
		printf("磁盘已满\n!");
		return 0;
	}
	int pIndex=getDiskIn();
	if(pIndex==-1)
	{
		printf("磁盘已满\n");
		return 0;
	}
	int te=getFileIndex(fileName);
	if(te!=-1)
	{
		printf("文件已存在\n");
		return 0;
	}

	if(!addDir(iNodep+cdirIn,fileName,nIndex))
		return 0;
	if(type==2)
		initDir(pIndex);
	strcpy((iNodep+nIndex)->fileName,fileName);
	(iNodep+nIndex)->fileSize=1;
	(iNodep+nIndex)->type=type;
	(iNodep+nIndex)->bSize=0;
	(iNodep+nIndex)->parent=cdirIn;
	(iNodep+nIndex)->wr=0;
	(iNodep+nIndex)->index[0]=pIndex;
	*(iNodeBP+nIndex)=1;
	*(pBP+pIndex)=1;

	return 1;
}

void remove2(int index)
{
	int nums;
	nums=(iNodep+index)->fileSize;
	for(int i=0;i<nums;i++)
		*(pBP+(iNodep+index)->index[i])=0;
	*(iNodeBP+index)=0;
}

int remove1(char* fileName)
{
	if(!fileName || strlen(fileName)>15)
	{
		printf("文件名格式错误\n");
		return 0;
	}
	int te=getFileIndex(fileName);
	if((iNodep+te)->type==2)
	{
		printf("请使用rmdir命令删除目录\n");
                return 0;
	}
	int nIndex=removeDir(iNodep+cdirIn,fileName);
        if(nIndex==-1)
	{
		printf("找不到该文件\n");
		return 0;
	}
	remove2(nIndex);
	return 1;
}

int mkdir1(char* dirName)
{
	return open2(dirName,2);
}

int open1(char* fileName)
{
	return open2(fileName,1);
}

void rmdirCore(int index)
{
	iNode* dir=iNodep+index;
	int nums=dir->fileSize;
	dirStruct buf[32];
        for(int i=0;i<nums;i++)
        {
                memcpy(buf,pp+(dir->index[i])*K,32*32);
                for(int j=0;j<32;j++)
                {
                        if(buf[j].index!=-1)
                        {
                                int ind=buf[j].index;
				if((iNodep+ind)->type==2)
					rmdirCore(ind);
				else
					remove2(ind);
                        }
                }
		*(pBP+dir->index[i])=0;
        }
	*(iNodeBP+index)=0;
}

int rmdir1(char* fileName)
{
	int ind=getFileIndex(fileName);
	if((iNodep+ind)->type==1)
	{
		printf("请使用rm命令删除文本文件\n");
		return 0;
	}
	if(ind!=-1)
	{
		rmdirCore(ind);
		removeDir(iNodep+cdirIn,fileName);
		return 1;
	}
	printf("文件不存在\n");
	return 0;
}

int read1(char* fileName,char* ch,int st,int n) //读取n个字节
{
	if(n<0 || n>4*K)
	{
		printf("读取字节数错误\n");
		return 0;
	}
	if(!n)
		return 1;
	char buf[K]; //缓冲数组
	int nIndex=getFileIndex(fileName);
	if(nIndex==-1)
	{
		printf("文件不存在\n");
		return 0;
	}
	if((iNodep+nIndex)->type==2)
	{
		printf("目录文件不可读写\n");
		return 0;
	}

	iNode* fileIn=iNodep+nIndex;
	int pIndex[4],nums=fileIn->fileSize,rei=st/K,reb=st%K,bs=fileIn->bSize;
//	printf("%d %d %d&&\n",st,bs,nums);
	if(bs<st+n)
        {
                printf("读取超过文件大小的内容\n");
                return 0;
        }
	for(int i=0;i<nums;i++)
		pIndex[i]=fileIn->index[i];

	int ren=0;
	int b[4];
	int ba=bs;
	for(int i=0;i<nums;i++)
	{
		b[i]=(ba>K ? K : ba);
		ba-=K;
	}
	for(int i=rei;i<nums && ren<n;i++)
	{
	//	printf("%d^\n",pIndex[i]);
		memcpy(buf,pp+pIndex[i]*K,K); //以盘块为单位读取数据到缓冲区中
		for(int j=reb;j<b[i] && ren<n;j++)
		{
	//		printf("%c ",buf[j]);
			ch[ren++]=buf[j];
		}
		reb=0;
	}
//	printf("\n");
	if(ren<n)
	{
		printf("%d %d读取超过文件大小的内容",rei,nums);
		return 0;
	}
	return 1;
}

int write1(char* fileName,char* ch,int n,int tag)
{
	if(n<0 || n>4*K)
	{
		printf("写入字节数错误\n");
		return 0;
	}
        if(!n)
                return 1;
	char buf[K]; //缓冲数组
	int nIndex=getFileIndex(fileName);
	int pIndex[4],nums,wri,wrb,bs;
	if(nIndex==-1)
                return 0;
	if((iNodep+nIndex)->type==2)
        {
                printf("目录文件不可读写\n");
                return 0;
        }

        iNode* fileIn=iNodep+nIndex;
	nums=fileIn->fileSize;
	if(tag==2)
	{
		fileIn->wr=0; //tag:1表示接着上次位置写，2表示从头开始写;
		fileIn->bSize=0;
	}
	if((fileIn->wr)<K)
	{
		for(int i=1;i<nums;i++)
		{
			int ai=fileIn->index[i];
                        *(pBP+ai)=0;
                        fileIn->fileSize=1;
		}
	}
	nums=fileIn->fileSize;
	int te=fileIn->wr;
	if(te+n>4*K)
	{
		printf("写入数据过大\n");
		return 0;
	}
	wri=te/K;
	wrb=te%K;
	bs=fileIn->bSize;
	for(int i=0;i<nums;i++)
		pIndex[i]=fileIn->index[i];

//	printf("%d %d %d(\n",te,wri,wrb);
	int wen=0;
	for(int i=wri;i<nums && wen<n;i++)
        {
                memcpy(buf,pp+pIndex[i]*K,K); //以盘块为单位读取数据到缓冲区中
                for(int j=wrb;j<K && wen<n;j++)
                        buf[j]=ch[wen++];
                wrb=0;
		memcpy(pp+pIndex[i]*K,buf,K);
        }
	while(wen<n)
	{
		if(nums==4)
		{
			printf("写入过大内容\n");
			return 0;
		}
		else
		{
			int pInd=getDiskIn();
			(iNodep+nIndex)->index[nums++]=pInd;
			(iNodep+nIndex)->fileSize=nums;
			*(pBP+pInd)=1;
			pIndex[nums-1]=pInd;

			//printf("%d^%d\n",pIndex[nums-1],wen);
			for(int i=0;i<K && wen<n;i++)
			{
				buf[i]=ch[wen++];
				printf("%c ",buf[i]);
			}
			printf("\n");
			memcpy(pp+pIndex[nums-1]*K,buf,K);
	       	}
	}
//	printf("*%d %d\n",pIndex[0],pIndex[1]);
	((iNodep+nIndex)->wr)+=wen;
        if((iNodep+nIndex)->wr==4*K)
                (iNodep+nIndex)->wr=0;
        ((iNodep+nIndex)->bSize)+=wen;
	return 1;
}

void create(char* ch,int n)
{
	srand((unsigned int)time(NULL));
	for(int i=0;i<n;i++)
	{
		int a=rand()%90+32;
		ch[i]=a;
	}
//	ch[n-1]=0;
}

int addFileSem(char* fileName)
{
	pthread_rwlock_wrlock(&rwlock2);
	for(int i=0;i<fsSize;i++)
        {
                if(!(fs+i)->isUsed)
                {
			(fs+i)->isUsed=1;
			strcpy((fs+i)->fileName,fileName);
			sem_init(&((fs+i)->rSem),0,1);
			sem_init(&((fs+i)->wSem),0,1);
			pthread_rwlock_unlock(&rwlock2);
                        return i;
                }
        }
	pthread_rwlock_unlock(&rwlock2);
	return -1;
}

int getFileSem(char* fileName)
{
	pthread_rwlock_rdlock(&rwlock2);
	for(int i=0;i<fsSize;i++)
        {
                if((fs+i)->isUsed && !strcmp((fs+i)->fileName,fileName))
                {
			pthread_rwlock_unlock(&rwlock2);
                        return i;
                }
        }
	pthread_rwlock_unlock(&rwlock2);

	int ind=addFileSem(fileName);
	if(ind==-1)
	{
		appendFs();
	        ind=addFileSem(fileName);
	}

	return ind;
}

void pclose1(int ind,sem_t* wp)
{
	sem_wait(wp);
	(fs+ind)->isUsed=0;
	deleteFs();
	sem_post(wp);
}

int pread1(char* fileName,char* ch,int st,int n)
{
	pthread_rwlock_rdlock(&rwlock1);

	sem_t* rp=0;
	sem_t* wp=0;
        int fsIn=getFileSem(fileName);
        if(fsIn==-1)
	{
		printf("磁盘已满\n");
		return 0;
	}
	rp=&((fs+fsIn)->rSem);
	wp=&((fs+fsIn)->wSem);

	sem_wait(rp);
	if((fs+fsIn)->readers==0)
		sem_wait(wp);
	(fs+fsIn)->readers++;
	sem_post(rp);

	int re=read1(fileName,ch,st,n);

	sem_wait(rp);
	((fs+fsIn)->readers)--;
	if((fs+fsIn)->readers==0)
		sem_post(wp);
	sem_post(rp);

	pclose1(fsIn,wp);

	pthread_rwlock_unlock(&rwlock1); 

	return re;
}

int pwrite1(char* fileName,char* ch,int n,int tag)
{
	pthread_rwlock_rdlock(&rwlock1);

	sem_t* rp=0;
	sem_t* wp=0;
        int fsIn=getFileSem(fileName);
        if(fsIn==-1)
        {
                printf("磁盘已满\n");
                return 0;
        }
        rp=&((fs+fsIn)->rSem);
        wp=&((fs+fsIn)->wSem);
 
	sem_wait(wp);

	int re;
        if(re=write1(fileName,ch,n,tag))
		;
               // printf("文件%s写入成功\n",fileName);

	sem_post(wp);

	pclose1(fsIn,wp);

	pthread_rwlock_unlock(&rwlock1);
	return re;
}

int popen1(char* fileName)
{
	pthread_rwlock_wrlock(&rwlock1);

	int re;
        if(re=open1(fileName))
		;
           // printf("创建文件%s成功\n",fileName);

        pthread_rwlock_unlock(&rwlock1);
	return re;
}

int premove1(char* fileName)
{
	pthread_rwlock_wrlock(&rwlock1);
	int re;
        if(re=remove1(fileName))
		;
        //    printf("删除文件%s成功\n",fileName);
        pthread_rwlock_unlock(&rwlock1);
        return re;
}

int pmkdir1(char* dirName)
{
	pthread_rwlock_wrlock(&rwlock1);
	int re=mkdir1(dirName);

	pthread_rwlock_unlock(&rwlock1);
	return re;
}

int pchangeDir(char* dirName)
{
	pthread_rwlock_wrlock(&rwlock1);
	int re=changeDir(dirName);

	pthread_rwlock_unlock(&rwlock1);
	return re;
}

int prmdir1(char* dirName)
{
	pthread_rwlock_wrlock(&rwlock1);
	int re=rmdir1(dirName);

	pthread_rwlock_unlock(&rwlock1);
	return re;
}

void pls1()
{
	pthread_rwlock_rdlock(&rwlock1);

	ls1();

	pthread_rwlock_unlock(&rwlock1);
}

int pchangeFileName(char* fileName1,char* fileName2)
{
	pthread_rwlock_rdlock(&rwlock1);

        int re=changeFileName(fileName1,fileName2);

        pthread_rwlock_unlock(&rwlock1);
	return re;
}

pthread_t tid1,tid2;
char cmd[2][16];
char name[16];
int mo,st,man;
void* pThreadRun(void* val)
{
	 if(!strcmp(cmd[0],"mkdir"))
        {
                if(pmkdir1(cmd[1]))
                        printf("目录创建成功\n");
        }
        else if(!strcmp(cmd[0],"cd"))
        {
                pchangeDir(cmd[1]);
        }
        else if(!strcmp(cmd[0],"rmdir"))
        {
               if( prmdir1(cmd[1]))
		       printf("删除目录成功\n");
        }
        else if(!strcmp(cmd[0],"rm"))
        {
                if(premove1(cmd[1]))
			printf("删除文件成功\n");
        }
        else if(!strcmp(cmd[0],"ls"))
        {
                pls1();
        }
        else if(!strcmp(cmd[0],"open"))
        {
                if(popen1(cmd[1]))
			printf("文件创建成功\n");
        }
	else if(!strcmp(cmd[0],"cn"))
        {
         //       printf("请输入修改后的名称: ");
           //     char name[16];
             //   scanf("%s",name);
                if(pchangeFileName(cmd[1],name))
                        printf("修改成功\n");
        }
	else if(!strcmp(cmd[0],"read"))
        {
        //        int st,man;
           //     printf("请输入开始字节以及读取字节数: ");
             //   scanf("%d %d",&st,&man);

                char f2[K*4];
                char re2[K*4];
                create(f2,man);

                if(pread1(cmd[1],re2+st,st,man))
                {
                        printf("文件%s读出成功\n",cmd[1]);
                        for(int i=st;i<st+man;i++)
                        {
                             //   if(re2[i]!=f2[i])
                               // {
                                 //       printf("内容错误");
                                   //     break;
                               // }
                                printf("%c ",re2[i]);
                        }
                        printf("\n");
                }
                else
                        printf("读取失败\n");
        }
        else if(!strcmp(cmd[0],"write"))
        {
 //               int mo,man;
   //             printf("请输入写入模式以及写入字节数: ");
     //           scanf("%d %d",&mo,&man);

                char f2[K*4];
                create(f2,man);

                if(pwrite1(cmd[1],f2,man,mo))
                        printf("写入成功\n");
                else
                        printf("写入失败\n");
        }
}

void solution(char cmd[2][16])
{
	if(!strcmp(cmd[0],"mkdir"))
	{
		if(mkdir1(cmd[1]))
			printf("目录创建成功\n");
	}
	else if(!strcmp(cmd[0],"cd"))
	{
		changeDir(cmd[1]);
	}
	else if(!strcmp(cmd[0],"rmdir"))
	{
		if(rmdir1(cmd[1]))
			printf("删除目录成功\n");
	}
	else if(!strcmp(cmd[0],"rm"))
	{
		if(remove1(cmd[1]))
			printf("删除文件成功\n");
	}
	else if(!strcmp(cmd[0],"ls"))
	{
		ls1();
	}
	else if(!strcmp(cmd[0],"open"))
	{
		if(open1(cmd[1]))
			printf("文件创建成功\n");
	}
	else if(!strcmp(cmd[0],"cn"))
	{
		printf("请输入修改后的名称: ");
		char name[16];
		scanf("%s",name);
		if(changeFileName(cmd[1],name))
			printf("修改成功\n");
	}
	else if(!strcmp(cmd[0],"read"))
	{
		int st,man;
		printf("请输入开始字节以及读取字节数: ");
		scanf("%d %d",&st,&man);

		char f2[K*4];
                char re2[K*4];
		create(f2,man);

		if(read1(cmd[1],re2+st,st,man))
                {
                        printf("文件%s读出成功\n",cmd[1]);
                        for(int i=st;i<st+man;i++)
                        {
                             //   if(re2[i]!=f2[i])
                               // {
                                 //       printf("内容错误");
                                   //     break;
                               // }
                                printf("%c ",re2[i]);
                        }
                        printf("\n");
                }
		else
			printf("读取失败\n");
	}
	else if(!strcmp(cmd[0],"write"))
	{
		int mo,man;
		printf("请输入写入模式以及写入字节数: ");
		scanf("%d %d",&mo,&man);

		char f2[K*4];
		create(f2,man);

		if(write1(cmd[1],f2,man,mo))
			printf("写入成功\n");
		else
			printf("写入失败\n");
	}
}

int main()
{
	init(); //初始化磁盘
	load(); //加载磁盘

	//char cmd[2][16];
	printf("%s:",dirSource);
	while(scanf("%s",cmd[0])!=EOF)
	{
		if(!strcmp(cmd[0],"pthread"))
		{
			printf("%s:",dirSource);
			while(scanf("%s",cmd[0])!=EOF)
			{
				if(!strcmp(cmd[0],"quit"))
					break;
				scanf("%s",cmd[1]);
				if(!strcmp(cmd[0],"read"))
				{
					printf("请输入开始字节以及读取字节数: ");
                                        scanf("%d %d",&st,&man);
				}
				else if(!strcmp(cmd[0],"write"))
				{
					printf("请输入写入模式以及写入字节数: ");
                                        scanf("%d %d",&mo,&man);
				}
				else if(!strcmp(cmd[0],"cn"))
                                {
                                        printf("请输入修改后的名称: ");
                               //         char name[16];
                                        scanf("%s",name);
                                        if(pchangeFileName(cmd[0],name))
                                                printf("修改成功\n");
                                }
				pthread_create(&tid1,NULL,pThreadRun,NULL);
				pthread_create(&tid2,NULL,pThreadRun,NULL);
                                pthread_join(tid1,NULL);
                                pthread_join(tid2,NULL);
				printf("%s:",dirSource);
			}
		}
		scanf("%s",cmd[1]);
		solution(cmd);
		printf("%s:",dirSource);
	}

/*	popen1("aaa");
        popen1("aaa");
        premove1("aaa");
        popen1("aaa");
        premove1("aaa");

	pthread_t tid1,tid2;
	char* f1="file1";
	char* f2="file2";
	pthread_create(&tid1,NULL,pThreadRun,f1);
	pthread_create(&tid2,NULL,pThreadRun,f2);
	pthread_join(tid1,NULL);
	pthread_join(tid2,NULL);*/

	return 0;
}

