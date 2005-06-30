
#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "include/config.h"

#include "mds/MDCluster.h"
#include "mds/MDS.h"
#include "osd/OSD.h"
#include "client/Client.h"
#include "client/SyntheticClient.h"

#include "msg/TCPMessenger.h"

#include "common/Timer.h"

#define NUMMDS g_conf.num_mds
#define NUMOSD g_conf.num_osd
#define NUMCLIENT g_conf.num_client

class C_Test : public Context {
public:
  void finish(int r) {
	cout << "C_Test->finish(" << r << ")" << endl;
  }
};


int main(int oargc, char **oargv) {

  //cerr << "tcpsyn starting " << myrank << "/" << world << endl;
  int argc;
  char **argv;
  parse_config_options(oargc, oargv,
					   argc, argv);

  int start = 0;

  // build new argc+argv for fuse
  typedef char* pchar;
  int nargc = 0;
  char **nargv = new pchar[argc];
  nargv[nargc++] = argv[0];

  string syn_sarg1;
  list<int> syn_modes;
  int syn_iarg1, syn_iarg2, syn_iarg3;
  int mkfs = 0;
  for (int i=1; i<argc; i++) {
	if (strcmp(argv[i], "--fastmkfs") == 0) {
	  mkfs = MDS_MKFS_FAST;
	}
	else if (strcmp(argv[i], "--fullmkfs") == 0) {
	  mkfs = MDS_MKFS_FULL;
	}
	else if (strcmp(argv[i],"--synsarg1") == 0) 
	  syn_sarg1 = argv[++i];
	else if (strcmp(argv[i],"--syniarg1") == 0) 
	  syn_iarg1 = atoi(argv[++i]);
	else if (strcmp(argv[i],"--syniarg2") == 0) 
	  syn_iarg2 = atoi(argv[++i]);
	else if (strcmp(argv[i],"--syniarg3") == 0) 
	  syn_iarg3 = atoi(argv[++i]);
	else if (strcmp(argv[i],"--synmode") == 0) {
	  ++i;
	  if (strcmp(argv[i],"writefile") == 0) 
		syn_modes.push_back( SYNCLIENT_MODE_WRITEFILE );
	  else if (strcmp(argv[i],"readfile") == 0) 
		syn_modes.push_back( SYNCLIENT_MODE_READFILE );
	  else if (strcmp(argv[i],"makedirs") == 0) 
		syn_modes.push_back( SYNCLIENT_MODE_MAKEDIRS );
	  else if (strcmp(argv[i],"fullwalk") == 0) 
		syn_modes.push_back( SYNCLIENT_MODE_FULLWALK );
	  else if (strcmp(argv[i],"randomwalk") == 0) 
		syn_modes.push_back( SYNCLIENT_MODE_RANDOMWALK );
	  else {
		cerr << "unknown syn mode " << argv[i] << endl;
		return -1;
	  }
	}

	else {
	  // unknown arg, pass it on.
	  nargv[nargc++] = argv[i];
	}
  }

  int myrank = tcpmessenger_init(argc, argv);
  int world = tcpmessenger_world();

  if (myrank == 0)
	cerr << "nummds " << NUMMDS << "  numosd " << NUMOSD << "  numclient " << NUMCLIENT << endl;
  assert(NUMMDS + NUMOSD + 1 <= world);

  MDCluster *mdc = new MDCluster(NUMMDS, NUMOSD);


  char hostname[100];
  gethostname(hostname,100);
  int pid = getpid();

  // create mds
  MDS *mds[NUMMDS];
  for (int i=0; i<NUMMDS; i++) {
	if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_MDS(i),world)) continue;
	cerr << "mds" << i << " on rank " << myrank << " " << hostname << "." << pid << endl;
	mds[i] = new MDS(mdc, i, new TCPMessenger(MSG_ADDR_MDS(i)));
	start++;
  }
  
  // create osd
  OSD *osd[NUMOSD];
  for (int i=0; i<NUMOSD; i++) {
	if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_OSD(i),world)) continue;
	cerr << "osd" << i << " on rank " << myrank << " " << hostname << "." << pid << endl;
	osd[i] = new OSD(i, new TCPMessenger(MSG_ADDR_OSD(i)));
	start++;
  }
  
  // create client
  set<int> clientlist;
  Client *client[NUMCLIENT];
  SyntheticClient *syn[NUMCLIENT];
  for (int i=0; i<NUMCLIENT; i++) {
	if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_CLIENT(i),world)) continue;
	clientlist.insert(i);
	client[i] = new Client(mdc, i, new TCPMessenger(MSG_ADDR_CLIENT(i)) );
	start++;
  }
  if (clientlist.size())
	cerr << "clients " << clientlist << " on rank " << myrank << " " << hostname << "." << pid << endl;


  // start message loop
  if (start) {
	tcpmessenger_start();
    
	// init
	for (int i=0; i<NUMMDS; i++) {
	  if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_MDS(i),world)) continue;
	  mds[i]->init();
	}
	
	for (int i=0; i<NUMOSD; i++) {
	  if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_OSD(i),world)) continue;
	  osd[i]->init();
	}
	
	// create client
	for (int i=0; i<NUMCLIENT; i++) {
	  if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_CLIENT(i),world)) continue;
	  
	  client[i]->init();
	  
	  // use my argc, argv (make sure you pass a mount point!)
	  //cout << "mounting" << endl;
	  client[i]->mount(mkfs);

	  //cout << "starting synthetic client on rank " << myrank << endl;
	  syn[i] = new SyntheticClient(client[i]);

	  char s[20];
	  sprintf(s,"syn.%d", i);
	  syn[i]->sarg1 = s;

	  syn[i]->modes = syn_modes;
	  syn[i]->iarg1 = syn_iarg1;
	  syn[i]->iarg2 = syn_iarg2;
	  syn[i]->iarg3 = syn_iarg3;
	  syn[i]->start_thread();
	}
	for (int i=0; i<NUMCLIENT; i++) {
	  if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_CLIENT(i),world)) continue;
	  
	  cout << "waiting for synthetic client" << i << " to finish" << endl;
	  syn[i]->join_thread();
	  delete syn[i];

	  client[i]->unmount();
	  //cout << "client" << i << " unmounted" << endl;
	  client[i]->shutdown();
	}
	
		
	// wait for it to finish
	tcpmessenger_wait();

	//assert(0);
  } else {
	cerr << "IDLE rank " << myrank << endl;
  }

  tcpmessenger_shutdown(); 
  
  // cleanup
  for (int i=0; i<NUMMDS; i++) {
	if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_MDS(i),world)) continue;
	delete mds[i];
  }
  for (int i=0; i<NUMOSD; i++) {
	if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_OSD(i),world)) continue;
	delete osd[i];
  }
  for (int i=0; i<NUMCLIENT; i++) {
	if (myrank != MPI_DEST_TO_RANK(MSG_ADDR_CLIENT(i),world)) continue;
	delete client[i];
  }
  delete mdc;
  
  return 0;
}

