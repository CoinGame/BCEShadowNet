// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2011-2013 The PPCoin developers
// Copyright (c) 2013-2014 The Peershares developers
// Copyright (c) 2014-2015 The Nu developers

// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "db.h"
#include "walletdb.h"
#include "bitcoinrpc.h"
#include "net.h"
#include "init.h"
#include "util.h"
#include "wallet.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <curl/curl.h>

#ifndef WIN32
#include <signal.h>
#endif

using namespace std;
using namespace boost;

//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//

void ExitTimeout(void* parg)
{
#ifdef WIN32
    Sleep(5000);
    ExitProcess(0);
#endif
}

void StartShutdown()
{
#ifdef QT_GUI
    // ensure we leave the Qt main loop for a clean GUI exit (Shutdown() is called in bitcoin.cpp afterwards)
    QueueShutdown();
#else
    // Without UI, Shutdown() can simply be started in a new thread
    CreateThread(Shutdown, NULL);
#endif
}

void Shutdown(void* parg)
{
    static CCriticalSection cs_Shutdown;
    static bool fTaken;
    bool fFirstThread = false;
    {
        TRY_LOCK(cs_Shutdown, lockShutdown);
        if (lockShutdown)
        {
            fFirstThread = !fTaken;
            fTaken = true;
        }
    }
    static bool fExit;
    if (fFirstThread)
    {
        fShutdown = true;
        nTransactionsUpdated++;
        DBFlush(false);
        StopNode();
        DBFlush(true);
        curl_global_cleanup();
        boost::filesystem::remove(GetPidFile());
        UnregisterAndDeleteAllWallets();
        CreateThread(ExitTimeout, NULL);
        Sleep(50);
        printf("B&C Exchange exiting\n\n");
        fExit = true;
#ifndef QT_GUI
        // ensure non UI client get's exited here, but let B&C Exchange-Qt reach return 0; in bitcoin.cpp
        exit(0);
#endif
    }
    else
    {
        while (!fExit)
            Sleep(500);
        Sleep(100);
        ExitThread(0);
    }
}

void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}






//////////////////////////////////////////////////////////////////////////////
//
// Start
//
#if !defined(QT_GUI)
int main(int argc, char* argv[])
{
    bool fRet = false;
    fRet = AppInit(argc, argv);

    if (fRet && fDaemon)
        return 0;

    return 1;
}
#endif

bool AppInit(int argc, char* argv[])
{
    bool fRet = false;
    try
    {
        fRet = AppInit2(argc, argv);
    }
    catch (std::exception& e) {
        PrintException(&e, "AppInit()");
    } catch (...) {
        PrintException(NULL, "AppInit()");
    }
    if (!fRet)
        Shutdown(NULL);
    return fRet;
}

bool AppInit2(int argc, char* argv[])
{
#ifdef _MSC_VER
    // Turn off microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
    // Disable confusing "helpful" text message on abort, ctrl-c
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifndef WIN32
    umask(077);
#endif
#ifndef WIN32
    // Clean shutdown on SIGTERM
    struct sigaction sa;
    sa.sa_handler = HandleSIGTERM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
#endif

    //
    // Parameters
    //
    // If Qt is used, parameters/nu.conf are parsed in qt/bitcoin.cpp's main()
#if !defined(QT_GUI)
    ParseParameters(argc, argv);
    if (!boost::filesystem::is_directory(GetDataDir(false)))
    {
        fprintf(stderr, "Error: Specified directory does not exist\n");
        Shutdown(NULL);
    }
    ReadConfigFile(mapArgs, mapMultiArgs);
    ReadPeercoinConfigFile(mapPeercoinArgs);
#endif

    if (mapArgs.count("-?") || mapArgs.count("--help"))
    {
        string strUsage = string() +
          _("B&C Exchange version") + " " + FormatFullVersion() + "\n\n" +
          _("Usage:") + "\t\t\t\t\t\t\t\t\t\t\n" +
            "  bcexchanged [options]                   \t  " + "\n" +
            "  bcexchanged [options] <command> [params]\t  " + _("Send command to -server or bcexchanged") + "\n" +
            "  bcexchanged [options] help              \t\t  " + _("List commands") + "\n" +
            "  bcexchanged [options] help <command>    \t\t  " + _("Get help for a command") + "\n" +
          _("Options:") + "\n" +
            "  -conf=<file>     \t\t  " + _("Specify configuration file (default: bcexchange.conf)") + "\n" +
            "  -pid=<file>      \t\t  " + _("Specify pid file (default: bcexchanged.pid)") + "\n" +
            "  -gen             \t\t  " + _("Generate coins") + "\n" +
            "  -gen=0           \t\t  " + _("Don't generate coins") + "\n" +
            "  -min             \t\t  " + _("Start minimized") + "\n" +
            "  -splash          \t\t  " + _("Show splash screen on startup (default: 1)") + "\n" +
            "  -datadir=<dir>   \t\t  " + _("Specify data directory") + "\n" +
            "  -dbcache=<n>     \t\t  " + _("Set database cache size in megabytes (default: 25)") + "\n" +
            "  -dblogsize=<n>   \t\t  " + _("Set database disk log size in megabytes (default: 100)") + "\n" +
            "  -timeout=<n>     \t  "   + _("Specify connection timeout (in milliseconds)") + "\n" +
            "  -proxy=<ip:port> \t  "   + _("Connect through socks4 proxy") + "\n" +
            "  -dns             \t  "   + _("Allow DNS lookups for addnode and connect") + "\n" +
            "  -port=<port>     \t\t  " + _("Listen for connections on <port> (default: 2239 or testnet: 12239)") + "\n" +
            "  -maxconnections=<n>\t  " + _("Maintain at most <n> connections to peers (default: 125)") + "\n" +
            "  -addnode=<ip>    \t  "   + _("Add a node to connect to and attempt to keep the connection open") + "\n" +
            "  -connect=<ip>    \t\t  " + _("Connect only to the specified node") + "\n" +
            "  -listen          \t  "   + _("Accept connections from outside (default: 1)") + "\n" +
#ifdef QT_GUI
            "  -lang=<lang>     \t\t  " + _("Set language, for example \"de_DE\" (default: system locale)") + "\n" +
#endif
            "  -dnsseed         \t  "   + _("Find peers using DNS lookup (default: 1)") + "\n" +
            "  -banscore=<n>    \t  "   + _("Threshold for disconnecting misbehaving peers (default: 100)") + "\n" +
            "  -bantime=<n>     \t  "   + _("Number of seconds to keep misbehaving peers from reconnecting (default: 86400)") + "\n" +
            "  -maxreceivebuffer=<n>\t  " + _("Maximum per-connection receive buffer, <n>*1000 bytes (default: 10000)") + "\n" +
            "  -maxsendbuffer=<n>\t  "   + _("Maximum per-connection send buffer, <n>*1000 bytes (default: 10000)") + "\n" +
#ifdef USE_UPNP
#if USE_UPNP
            "  -upnp            \t  "   + _("Use Universal Plug and Play to map the listening port (default: 1)") + "\n" +
#else
            "  -upnp            \t  "   + _("Use Universal Plug and Play to map the listening port (default: 0)") + "\n" +
#endif
            "  -detachdb        \t  "   + _("Detach block and address databases. Increases shutdown time (default: 0)") + "\n" +
#endif
#ifdef QT_GUI
            "  -server          \t\t  " + _("Accept command line and JSON-RPC commands") + "\n" +
#endif
#if !defined(WIN32) && !defined(QT_GUI)
            "  -daemon          \t\t  " + _("Run in the background as a daemon and accept commands") + "\n" +
#endif
            "  -testnet         \t\t  " + _("Use the test network") + "\n" +
            "  -debug           \t\t  " + _("Output extra debugging information") + "\n" +
            "  -logtimestamps   \t  "   + _("Prepend debug output with timestamp") + "\n" +
            "  -printtoconsole  \t  "   + _("Send trace/debug info to console instead of debug.log file") + "\n" +
#ifdef WIN32
            "  -printtodebugger \t  "   + _("Send trace/debug info to debugger") + "\n" +
#endif
            "  -rpcuser=<user>  \t  "   + _("Username for JSON-RPC connections") + "\n" +
            "  -rpcpassword=<pw>\t  "   + _("Password for JSON-RPC connections") + "\n" +
            "  -rpcport=<port>  \t\t  " + _("Listen for JSON-RPC connections on <port> (default: 2240 or testnet: 12240)") + "\n" +
            "  -rpcallowip=<ip> \t\t  " + _("Allow JSON-RPC connections from specified IP address") + "\n" +
            "  -rpcconnect=<ip> \t  "   + _("Send commands to node running on <ip> (default: 127.0.0.1)") + "\n" +
            "  -blocknotify=<cmd> "     + _("Execute command when the best block changes (%s in cmd is replaced by block hash)") + "\n" +
            "  -walletnotify=<cmd> "    + _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)") + "\n" +
            "  -votenotify=<cmd>  "     + _("Execute command when the vote changes") + "\n" +
            "  -upgradewallet   \t  "   + _("Upgrade wallet to latest format") + "\n" +
            "  -keypool=<n>     \t  "   + _("Set key pool size to <n> (default: 100)") + "\n" +
            "  -rescan          \t  "   + _("Rescan the block chain for missing wallet transactions") + "\n" +
            "  -checkblocks=<n> \t\t  " + _("How many blocks to check at startup (default: 2500, 0 = all)") + "\n" +
            "  -checklevel=<n>  \t\t  " + _("How thorough the block verification is (0-6, default: 1)") + "\n";

        strUsage += string() +
            _("\nSSL options: (see the B&C Exchange Wiki for SSL setup instructions)") + "\n" +
            "  -rpcssl                                \t  " + _("Use OpenSSL (https) for JSON-RPC connections") + "\n" +
            "  -rpcsslcertificatechainfile=<file.cert>\t  " + _("Server certificate file (default: server.cert)") + "\n" +
            "  -rpcsslprivatekeyfile=<file.pem>       \t  " + _("Server private key (default: server.pem)") + "\n" +
            "  -rpcsslciphers=<ciphers>               \t  " + _("Acceptable ciphers (default: TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!AH:!3DES:@STRENGTH)") + "\n";

        strUsage += string() +
            "  -?               \t\t  " + _("This help message") + "\n";

        // Remove tabs
        strUsage.erase(std::remove(strUsage.begin(), strUsage.end(), '\t'), strUsage.end());
#if defined(QT_GUI) && defined(WIN32)
        // On windows, show a message box, as there is no stderr
        ThreadSafeMessageBox(strUsage, _("Usage"), wxOK | wxMODAL);
#else
        fprintf(stderr, "%s", strUsage.c_str());
#endif
        return false;
    }

    curl_global_init(CURL_GLOBAL_NOTHING);

#ifdef TESTING
    if (mapArgs.count("-timetravel"))
        nTimeShift = GetArg("-timetravel", 0);
#endif

    fTestNet = GetBoolArg("-testnet");
    if (fTestNet)
    {
        SoftSetBoolArg("-irc", true);
    }

    fDebug = GetBoolArg("-debug");
    fDetachDB = GetBoolArg("-detachdb", false);

#if !defined(WIN32) && !defined(QT_GUI)
    fDaemon = GetBoolArg("-daemon");
#else
    fDaemon = false;
#endif

    if (fDaemon)
        fServer = true;
    else
        fServer = GetBoolArg("-server");

    /* force fServer when running without GUI */
#if !defined(QT_GUI)
    fServer = true;
#endif
    fPrintToConsole = GetBoolArg("-printtoconsole");
    fPrintToDebugger = GetBoolArg("-printtodebugger");
    fLogTimestamps = GetBoolArg("-logtimestamps");

#ifndef QT_GUI
    for (int i = 1; i < argc; i++)
        if (!IsSwitchChar(argv[i][0]) && !(strlen(argv[i]) >= 11 && strncasecmp(argv[i], "B&C Exchange:", 13) == 0))
            fCommandLine = true;

    if (fCommandLine)
    {
        int ret = CommandLineRPC(argc, argv);
        exit(ret);
    }
#endif

#if !defined(WIN32) && !defined(QT_GUI)
    if (fDaemon)
    {
        // Daemonize
        pid_t pid = fork();
        if (pid < 0)
        {
            fprintf(stderr, "Error: fork() returned %d errno %d\n", pid, errno);
            return false;
        }
        if (pid > 0)
        {
            CreatePidFile(GetPidFile(), pid);
            return true;
        }

        pid_t sid = setsid();
        if (sid < 0)
            fprintf(stderr, "Error: setsid() returned %d errno %d\n", sid, errno);
    }
#endif

    if (!fDebug)
        ShrinkDebugFile();
    printf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    printf("B&C Exchange version %s (%s)\n", FormatFullVersion().c_str(), CLIENT_DATE.c_str());
    printf("Default data directory %s\n", GetDefaultDataDir().string().c_str());

    if (GetBoolArg("-loadblockindextest"))
    {
        CTxDB txdb("r");
        txdb.LoadBlockIndex();
        PrintBlockTree();
        return false;
    }

    // Make sure only a single B&C Exchange process is using the data directory.
    boost::filesystem::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fopen(pathLockFile.string().c_str(), "a"); // empty lock file; created if it doesn't exist.
    if (file) fclose(file);
    static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
    if (!lock.try_lock())
    {
        ThreadSafeMessageBox(strprintf(_("Cannot obtain a lock on data directory %s.  B&C Exchange is probably already running."), GetDataDir().string().c_str()), _("B&C Exchange"), wxOK|wxMODAL);
        return false;
    }

    std::ostringstream strErrors;
    //
    // Load data files
    //
    if (fDaemon)
        fprintf(stdout, "B&C Exchange server starting\n");
    int64 nStart;

    InitMessage(_("Loading addresses..."));
    printf("Loading addresses...\n");
    nStart = GetTimeMillis();
    if (!LoadAddresses())
        strErrors << _("Error loading addr.dat") << "\n";
    printf(" addresses   %15"PRI64d"ms\n", GetTimeMillis() - nStart);

    InitMessage(_("Loading block index..."));
    printf("Loading block index...\n");
    nStart = GetTimeMillis();
    if (!LoadBlockIndex())
        strErrors << _("Error loading blkindex.dat") << "\n";

    // as LoadBlockIndex can take several minutes, it's possible the user
    // requested to kill B&C Exchange-Qt during the last operation. If so, exit.
    // As the program has not fully started yet, Shutdown() is possibly overkill.
    if (fRequestShutdown)
    {
        printf("Shutdown requested. Exiting.\n");
        return false;
    }
    printf(" block index %15"PRI64d"ms\n", GetTimeMillis() - nStart);

    InitMessage(_("Loading wallets..."));
    printf("Loading portfolio...\n");
    nStart = GetTimeMillis();

    BOOST_FOREACH(unsigned char unit, sAvailableUnits)
    {
        printf("Loading wallet for unit %c...\n", unit);
        nStart = GetTimeMillis();
        bool fFirstRun;
        string walletFilename((format("wallet%c.dat") % unit).str());

        CWallet* pwalletMain;
        pwalletMain = new CWallet(walletFilename);
        pwalletMain->SetUnit(unit);

        int nLoadWalletRet = pwalletMain->LoadWallet(fFirstRun);
        if (nLoadWalletRet != DB_LOAD_OK)
        {
            if (nLoadWalletRet == DB_CORRUPT)
                strErrors << format(_("Error loading %s: Wallet corrupted")) % walletFilename << "\n";
            else if (nLoadWalletRet == DB_TOO_NEW)
                strErrors << format(_("Error loading %s: Wallet requires newer version of B&C Exchange")) % walletFilename << "\n";
            else if (nLoadWalletRet == DB_NEED_REWRITE)
            {
                strErrors << _("Wallet needed to be rewritten: restart B&C Exchange to complete") << "\n";
                printf("%s", strErrors.str().c_str());
                ThreadSafeMessageBox(strErrors.str(), _("B&C Exchange"), wxOK | wxICON_ERROR | wxMODAL);
                return false;
            }
            else
                strErrors << format(_("Error loading %s")) % walletFilename << "\n";
        }

        if (GetBoolArg("-upgradewallet", fFirstRun))
        {
            int nMaxVersion = GetArg("-upgradewallet", 0);
            if (nMaxVersion == 0) // the -walletupgrade without argument case
            {
                printf("Performing wallet upgrade to %i\n", FEATURE_LATEST);
                nMaxVersion = CLIENT_VERSION;
                pwalletMain->SetMinVersion(FEATURE_LATEST); // permanently upgrade the wallet immediately
            }
            else
                printf("Allowing wallet upgrade up to %i\n", nMaxVersion);
            if (nMaxVersion < pwalletMain->GetVersion())
                strErrors << _("Cannot downgrade wallet") << "\n";
            pwalletMain->SetMaxVersion(nMaxVersion);
        }

    if (fFirstRun)
    {
        // Create new keyUser and set as default key
        RandAddSeedPerfmon();

        CPubKey newDefaultKey;
        if (!pwalletMain->GetKeyFromPool(newDefaultKey, false))
            strErrors << _("Cannot initialize keypool") << "\n";
        pwalletMain->SetDefaultKey(newDefaultKey);
        if (!pwalletMain->SetAddressBookName(pwalletMain->vchDefaultKey.GetID(), ""))
            strErrors << _("Cannot write default address") << "\n";
    }

        printf("%s", strErrors.str().c_str());
        printf(" wallet      %15"PRI64d"ms\n", GetTimeMillis() - nStart);

        RegisterWallet(pwalletMain);

        CBlockIndex *pindexRescan = pindexBest;
        if (GetBoolArg("-rescan"))
            pindexRescan = pindexGenesisBlock;
        else
        {
            CWalletDB walletdb(walletFilename);
            CBlockLocator locator;
            if (walletdb.ReadBestBlock(locator))
                pindexRescan = locator.GetBlockIndex();
        }
        if (pindexBest != pindexRescan && pindexBest && pindexRescan && pindexBest->nHeight > pindexRescan->nHeight)
        {
            InitMessage(_("Rescanning..."));
            printf("Rescanning last %i blocks (from block %i)...\n", pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight);
            nStart = GetTimeMillis();
            pwalletMain->ScanForWalletTransactions(pindexRescan, true);
            printf(" rescan      %15"PRI64d"ms\n", GetTimeMillis() - nStart);
        }

        InitMessage(_("Done loading"));
        printf("Done loading\n");

        //// debug print
        printf("mapBlockIndex.size() = %d\n",   mapBlockIndex.size());
        printf("nBestHeight = %d\n",            nBestHeight);
        printf("setKeyPool.size() = %d\n",      pwalletMain->setKeyPool.size());
        printf("mapWallet.size() = %d\n",       pwalletMain->mapWallet.size());
        printf("mapAddressBook.size() = %d\n",  pwalletMain->mapAddressBook.size());
    }

    if (!strErrors.str().empty())
    {
        ThreadSafeMessageBox(strErrors.str(), _("B&C Exchange"), wxOK | wxICON_ERROR | wxMODAL);
        return false;
    }

    BOOST_FOREACH(CWallet* wallet, setpwalletRegistered)
    {
        // Add wallet transactions that aren't already in a block to mapTransactions
        wallet->ReacceptWalletTransactions();
    }

    // Note: B&C Exchange-Qt stores several settings in the wallet, so we want
    // to load the wallet BEFORE parsing command-line arguments, so
    // the command-line/nu.conf settings override GUI setting.

    //
    // Parameters
    //
    if (GetBoolArg("-printblockindex") || GetBoolArg("-printblocktree"))
    {
        PrintBlockTree();
        return false;
    }

    if (mapArgs.count("-timeout"))
    {
        int nNewTimeout = GetArg("-timeout", 5000);
        if (nNewTimeout > 0 && nNewTimeout < 600000)
            nConnectTimeout = nNewTimeout;
    }

    if (mapArgs.count("-printblock"))
    {
        string strMatch = mapArgs["-printblock"];
        int nFound = 0;
        for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
        {
            uint256 hash = (*mi).first;
            if (strncmp(hash.ToString().c_str(), strMatch.c_str(), strMatch.size()) == 0)
            {
                CBlockIndex* pindex = (*mi).second;
                CBlock block;
                block.ReadFromDisk(pindex);
                block.BuildMerkleTree();
                block.print();
                printf("\n");
                nFound++;
            }
        }
        if (nFound == 0)
            printf("No blocks matching %s were found\n", strMatch.c_str());
        return false;
    }

    if (mapArgs.count("-proxy"))
    {
        fUseProxy = true;
        addrProxy = CService(mapArgs["-proxy"], 9050);
        if (!addrProxy.IsValid())
        {
            ThreadSafeMessageBox(_("Invalid -proxy address"), _("B&C Exchange"), wxOK | wxMODAL);
            return false;
        }
    }

    bool fTor = (fUseProxy && addrProxy.GetPort() == 9050);
    if (fTor)
    {
        // Use SoftSetBoolArg here so user can override any of these if they wish.
        // Note: the GetBoolArg() calls for all of these must happen later.
        SoftSetBoolArg("-listen", false);
        SoftSetBoolArg("-irc", false);
        SoftSetBoolArg("-dnsseed", false);
        SoftSetBoolArg("-upnp", false);
        SoftSetBoolArg("-dns", false);
    }

    fAllowDNS = GetBoolArg("-dns");
    fNoListen = !GetBoolArg("-listen", true);

    if (mapArgs.count("-splitshareoutputs"))
        nSplitShareOutputs = boost::lexical_cast<double>(mapArgs["-splitshareoutputs"]) * COIN;
    else
        nSplitShareOutputs = MIN_COINSTAKE_VALUE;

    // Continue to put "/P2SH/" in the coinbase to monitor
    // BIP16 support.
    // This can be removed eventually...
    const char* pszP2SH = "/P2SH/";
    COINBASE_FLAGS << std::vector<unsigned char>(pszP2SH, pszP2SH+strlen(pszP2SH));

    if (!fNoListen)
    {
        std::string strError;
        if (!BindListenPort(strError))
        {
            ThreadSafeMessageBox(strError, _("B&C Exchange"), wxOK | wxMODAL);
            return false;
        }
    }

    if (mapArgs.count("-addnode"))
    {
        BOOST_FOREACH(string strAddr, mapMultiArgs["-addnode"])
        {
            CAddress addr(CService(strAddr, GetDefaultPort(), fAllowDNS));
            addr.nTime = 0; // so it won't relay unless successfully connected
            if (addr.IsValid())
                addrman.Add(addr, CNetAddr("127.0.0.1"));
        }
    }

    if (mapArgs.count("-reservebalance")) // B&C Exchange: reserve balance amount
    {
        int64 nReserveBalance = 0;
        if (!ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        {
            ThreadSafeMessageBox(_("Invalid amount for -reservebalance=<amount>"), _("B&C Exchange"), wxOK | wxMODAL);
            return false;
        }
    }

    if (mapArgs.count("-checkpointkey")) // B&C Exchange: checkpoint master priv key
    {
        if (!Checkpoints::SetCheckpointPrivKey(GetArg("-checkpointkey", "")))
            ThreadSafeMessageBox(_("Unable to sign checkpoint, wrong checkpointkey?\n"), _("B&C Exchange"), wxOK | wxMODAL);
    }

    //
    // Start the node
    //
    if (!CheckDiskSpace())
        return false;

    RandAddSeedPerfmon();

    if (!CreateThread(StartNode, NULL))
        ThreadSafeMessageBox(_("Error: CreateThread(StartNode) failed"), _("B&C Exchange"), wxOK | wxMODAL);

    if (fServer)
    {
        BOOST_FOREACH(CWallet *wallet, setpwalletRegistered)
            CreateThread(ThreadRPCServer, wallet);
    }

#ifdef QT_GUI
    if (GetStartOnSystemStartup())
        SetStartOnSystemStartup(true); // Remove startup links
#endif

#if !defined(QT_GUI)
    while (1)
        Sleep(5000);
#endif

    return true;
}

