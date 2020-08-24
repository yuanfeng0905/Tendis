// Copyright [2017] <eliotwang>
#include <memory>
#include <utility>
#include <thread>
#include "gtest/gtest.h"
#include "glog/logging.h"
#include "tendisplus/server/server_entry.h"
#include "tendisplus/server/index_manager.h"
#include "tendisplus/server/segment_manager.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/network/network.h"
#include "tendisplus/utils/test_util.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/sync_point.h"
#include "tendisplus/utils/invariant.h"

namespace tendisplus {

std::string master_dir = "repltest_master";
std::string slave_dir = "repltest_slave";
std::string slave_slave_dir = "repltest_slave_slave";
std::string slave1_dir = "repltest_slave1";
std::string slave2_dir = "repltest_slave2";
std::string single_dir = "repltest_single";
uint32_t master_port = 1111;
uint32_t slave_port = 1112;
uint32_t slave_slave_port = 1113;
uint32_t slave1_port = 2111;
uint32_t slave2_port = 2112;
uint32_t single_port = 2113;

AllKeys initData(std::shared_ptr<ServerEntry>& server,
                uint32_t count) {
    auto ctx1 = std::make_shared<asio::io_context>();
    auto sess1 = makeSession(server, ctx1);
    WorkLoad work(server, sess1);
    work.init();

    AllKeys all_keys;

    auto kv_keys = work.writeWork(RecordType::RT_KV, count);
    all_keys.emplace_back(kv_keys);

    auto list_keys = work.writeWork(RecordType::RT_LIST_META, count, 50);
    all_keys.emplace_back(list_keys);

//#ifdef _WIN32
    work.flush();
//#endif

    auto hash_keys = work.writeWork(RecordType::RT_HASH_META, count, 50);
    all_keys.emplace_back(hash_keys);

    auto set_keys = work.writeWork(RecordType::RT_SET_META, count, 50);
    all_keys.emplace_back(set_keys);

    auto zset_keys = work.writeWork(RecordType::RT_ZSET_META, count, 50);
    all_keys.emplace_back(zset_keys);

    return std::move(all_keys);
}

void compareData(const std::shared_ptr<ServerEntry>& master,
    const std::shared_ptr<ServerEntry>& slave) {
    INVARIANT(master->getKVStoreCount() == slave->getKVStoreCount());

    for (size_t i = 0; i < master->getKVStoreCount(); i++) {
        uint64_t count1 = 0;
        uint64_t count2 = 0;
        auto kvstore1 = master->getStores()[i];
        auto kvstore2 = slave->getStores()[i];

        auto ptxn2 = kvstore2->createTransaction(nullptr);
        EXPECT_TRUE(ptxn2.ok());
        std::unique_ptr<Transaction> txn2 = std::move(ptxn2.value());

        auto ptxn1 = kvstore1->createTransaction(nullptr);
        EXPECT_TRUE(ptxn1.ok());
        std::unique_ptr<Transaction> txn1 = std::move(ptxn1.value());
        auto cursor1 = txn1->createCursor();
        cursor1->seek("");
        while (true) {
            Expected<Record> exptRcd1 = cursor1->next();
            if (exptRcd1.status().code() == ErrorCodes::ERR_EXHAUST) {
                break;
            }
            INVARIANT(exptRcd1.ok());
            count1++;

            // check the binlog together
            auto exptRcdv2 = kvstore2->getKV(exptRcd1.value().getRecordKey(), txn2.get());
            EXPECT_TRUE(exptRcdv2.ok());
            if (!exptRcdv2.ok()) {
                LOG(INFO) << exptRcd1.value().toString()
                    << " error:" << exptRcdv2.status().toString();
                continue;
            }
            EXPECT_EQ(exptRcd1.value().getRecordValue(), exptRcdv2.value());
        }

        auto cursor2 = txn2->createCursor();
        cursor2->seek("");
        while (true) {
            Expected<Record> exptRcd2 = cursor2->next();
            if (exptRcd2.status().code() == ErrorCodes::ERR_EXHAUST) {
                break;
            }
            INVARIANT(exptRcd2.ok());
            count2++;

            // check the binlog together
            auto exptRcdv1 = kvstore1->getKV(exptRcd2.value().getRecordKey(), txn1.get());
            EXPECT_TRUE(exptRcdv1.ok());
            if (!exptRcdv1.ok()) {
                LOG(INFO) << exptRcd2.value().toString()
                    << " error:" << exptRcdv1.status().toString();
                continue;
            }
            EXPECT_EQ(exptRcd2.value().getRecordValue(), exptRcdv1.value());
        }

        EXPECT_EQ(count1, count2);
        LOG(INFO) << "compare data: store " << i << " record count " << count1;
    }
}

std::pair<std::shared_ptr<ServerEntry>, std::shared_ptr<ServerEntry>> 
makeReplEnv(uint32_t storeCnt) {
    EXPECT_TRUE(setupEnv(master_dir));
    EXPECT_TRUE(setupEnv(slave_dir));

    auto cfg1 = makeServerParam(master_port, storeCnt, master_dir, false);
    auto cfg2 = makeServerParam(slave_port, storeCnt, slave_dir, false);

    auto master = std::make_shared<ServerEntry>(cfg1);
    auto s = master->startup(cfg1);
    INVARIANT(s.ok());

    auto slave = std::make_shared<ServerEntry>(cfg2);
    s = slave->startup(cfg2);
    INVARIANT(s.ok());
    {
        auto ctx = std::make_shared<asio::io_context>();
        auto session = makeSession(slave, ctx);

        WorkLoad work(slave, session);
        work.init();
        work.slaveof("127.0.0.1", master_port);
    }

    return std::make_pair(master, slave);
}

std::shared_ptr<ServerEntry>
makeAnotherSlave(const std::string& name, uint32_t storeCnt, uint32_t port, uint32_t mport) {
    INVARIANT(name != master_dir && name != slave_dir);
	INVARIANT(port != mport && port != slave_port);
    EXPECT_TRUE(setupEnv(name));

    auto cfg1 = makeServerParam(port, storeCnt, name, false);

    auto slave = std::make_shared<ServerEntry>(cfg1);
    auto s = slave->startup(cfg1);
    INVARIANT(s.ok());

    {
        auto ctx = std::make_shared<asio::io_context>();
        auto session = makeSession(slave, ctx);

        WorkLoad work(slave, session);
        work.init();
        work.slaveof("127.0.0.1", mport);
    }

    return slave;
}

#ifdef _WIN32 
size_t recordSize = 10;
#else
size_t recordSize = 1000;
#endif

TEST(Repl, Common) {
#ifdef _WIN32
    size_t i = 0;
    {
#else
    for(size_t i = 0; i<9; i++) {
#endif
        LOG(INFO) << ">>>>>> test store count:" << i;
        const auto guard = MakeGuard([] {
                destroyReplEnv();
                destroyEnv(slave1_dir);
                destroyEnv(slave2_dir);
                std::this_thread::sleep_for(std::chrono::seconds(5));
                });

        auto hosts = makeReplEnv(i);

        auto& master = hosts.first;
        auto& slave = hosts.second;
        bool running = true;

        // make sure slaveof is ok
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto ctx1 = std::make_shared<asio::io_context>();
        auto sess1 = makeSession(master, ctx1);
        WorkLoad work(master, sess1);
        work.init();
        work.flush();

        auto allKeys = initData(master, recordSize);

        waitSlaveCatchup(master, slave);
        sleep(3); // wait recycle binlog
        compareData(master, slave);

        auto slave1 = makeAnotherSlave(slave1_dir, i, slave1_port, master_port);
        // chain slave
        auto slave_slave = makeAnotherSlave(slave_slave_dir, i, slave_slave_port, slave_port);

        std::thread thd0([&master, &slave, &slave1, &slave_slave, &running]() {
            while (running) {
                auto s = runCommand(master, { "info", "replication" });
                LOG(INFO) << s;
                s = runCommand(slave, { "info", "replication" });
                LOG(INFO) << s;
                s = runCommand(slave_slave, { "info", "replication" });
                LOG(INFO) << s;
                s = runCommand(slave1, { "info", "replication" });
                LOG(INFO) << s;

                runBgCommand(master);
                runBgCommand(slave);
                runBgCommand(slave_slave);
                runBgCommand(slave1);

                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });

        // delete all the keys
        for (auto k : allKeys) {
            if (genRand() % 4 == 0) {
                work.delKeys(k);
            }
        }

        std::thread thd1([&master]() {
#ifndef _WIN32
            testAll(master);
#endif
        });

        std::thread thd2([&master]() {
#ifndef _WIN32
            std::this_thread::sleep_for(std::chrono::seconds(genRand() % 50));
            auto ctx1 = std::make_shared<asio::io_context>();
            auto sess1 = makeSession(master, ctx1);
            WorkLoad work(master, sess1);
            work.init();
            //work.flush();
#endif
        });


        std::this_thread::sleep_for(std::chrono::seconds(genRand() % 10 + 5));
        auto slave2 = makeAnotherSlave(slave2_dir, i, slave2_port, master_port);

        LOG(INFO) << "waiting thd1 to exited";
        running = false;
        thd0.join();
        thd1.join();
        thd2.join();

        sleep(3); // wait recycle binlog

        waitSlaveCatchup(master, slave);
        compareData(master, slave);

        waitSlaveCatchup(master, slave1);
        compareData(master, slave1);

        waitSlaveCatchup(master, slave2);
        compareData(master, slave2);

        waitSlaveCatchup(slave, slave_slave);
        compareData(slave, slave_slave);

#ifndef _WIN32
        master->stop();
        slave->stop();
        slave_slave->stop();
        slave1->stop();
        slave2->stop();

        ASSERT_EQ(slave.use_count(), 1);
        //ASSERT_EQ(master.use_count(), 1);
        ASSERT_EQ(slave_slave.use_count(), 1);
        ASSERT_EQ(slave1.use_count(), 1);
        ASSERT_EQ(slave2.use_count(), 1);
#endif

        LOG(INFO) << ">>>>>> test store count:" << i << " end;";
    }
}

void runCmd(std::shared_ptr<ServerEntry> svr, const std::vector<std::string>& args) {
    auto ctx = std::make_shared<asio::io_context>();
    auto session = makeSession(svr, ctx);
    session->setArgs(args);
    auto expect = Command::runSessionCmd(session.get());
    EXPECT_EQ(expect.ok(), true);
}

TEST(Repl, SlaveOfNeedEmpty) {
    size_t i = 0;
    {
        LOG(INFO) << ">>>>>> test store count:" << i;
        const auto guard = MakeGuard([] {
            destroyEnv(master_dir);
            destroyEnv(slave_dir);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        });

        EXPECT_TRUE(setupEnv(master_dir));
        EXPECT_TRUE(setupEnv(slave_dir));

        auto cfg1 = makeServerParam(master_port, i, master_dir, false);
        auto cfg2 = makeServerParam(slave_port, i, slave_dir, false);

        auto master = std::make_shared<ServerEntry>(cfg1);
        auto s = master->startup(cfg1);
        INVARIANT(s.ok());

        auto slave = std::make_shared<ServerEntry>(cfg2);
        s = slave->startup(cfg2);
        INVARIANT(s.ok());
        {
            auto ctx0 = std::make_shared<asio::io_context>();
            auto session0 = makeSession(master, ctx0);
            session0->setArgs({ "set", "a", "2" });
            auto expect = Command::runSessionCmd(session0.get());
            EXPECT_TRUE(expect.ok());

            auto ctx = std::make_shared<asio::io_context>();
            auto session = makeSession(slave, ctx);

            session->setArgs({ "set", "a", "1" });
            expect = Command::runSessionCmd(session.get());
            EXPECT_TRUE(expect.ok());

            session->setArgs({ "slaveof", "127.0.0.1", std::to_string(master_port) });
            expect = Command::runSessionCmd(session.get());
            EXPECT_EQ(expect.ok(), false);
            EXPECT_EQ(expect.status().toString(), "-ERR:4,msg:store not empty\r\n");

            session->setArgs({ "flushall", });
            expect = Command::runSessionCmd(session.get());
            EXPECT_EQ(expect.ok(), true);

            session->setArgs({ "slaveof", "127.0.0.1", std::to_string(master_port) });
            expect = Command::runSessionCmd(session.get());
            EXPECT_EQ(expect.ok(), true);

            waitSlaveCatchup(master, slave);

            session->setArgs({ "get", "a" });
            expect = Command::runSessionCmd(session.get());
            EXPECT_EQ(expect.value(), "$1\r\n2\r\n" );
        }

#ifndef _WIN32
        master->stop();
        slave->stop();

        ASSERT_EQ(slave.use_count(), 1);
#endif

        LOG(INFO) << ">>>>>> test store count:" << i << " end;";
    }
}

void checkBinlogFile(string dir, bool hasBinlog, uint32_t storeCount) {
    for (uint32_t i = 0; i < storeCount; ++i) {
        std::string fullFileName = dir + "/dump/" + std::to_string(i) + "/";

        uint32_t maxsize = 0;
        for (auto&DirectoryIter : filesystem::directory_iterator(fullFileName))
        {
            uint32_t size = filesystem::file_size(DirectoryIter.path());
            LOG(INFO) << "checkBinlogFile, path:" << DirectoryIter.path() << " size:" << size;
            if (size > maxsize) {
                maxsize = size;
            }
        }
        if (hasBinlog) {
            EXPECT_GT(maxsize, BINLOG_HEADER_V2_LEN);
        } else {
            EXPECT_TRUE(maxsize == BINLOG_HEADER_V2_LEN || maxsize == 0);
        }
    }
}

TEST(Repl, MasterDontSaveBinlog) {
    size_t i = 1;
    {
        LOG(INFO) << ">>>>>> test store count:" << i;
        const auto guard = MakeGuard([] {
            destroyEnv(master_dir);
            destroyEnv(slave_dir);
            destroyEnv(slave1_dir);
            destroyEnv(single_dir);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        });

        EXPECT_TRUE(setupEnv(master_dir));
        EXPECT_TRUE(setupEnv(slave_dir));
        EXPECT_TRUE(setupEnv(slave1_dir));
        EXPECT_TRUE(setupEnv(single_dir));

        auto cfg1 = makeServerParam(master_port, i, master_dir, false);
        auto cfg2 = makeServerParam(slave_port, i, slave_dir, false);
        auto cfg3 = makeServerParam(slave1_port, i, slave1_dir, false);
        auto cfg4 = makeServerParam(single_port, i, single_dir, false);
        cfg1->maxBinlogKeepNum = 1;
        cfg2->maxBinlogKeepNum = 1;
        cfg2->slaveBinlogKeepNum = 1;
        cfg3->maxBinlogKeepNum = 1;
        cfg3->slaveBinlogKeepNum = 1;
        cfg4->maxBinlogKeepNum = 1;

        auto master = std::make_shared<ServerEntry>(cfg1);
        auto s = master->startup(cfg1);
        INVARIANT(s.ok());

        auto slave = std::make_shared<ServerEntry>(cfg2);
        s = slave->startup(cfg2);
        INVARIANT(s.ok());

        auto slave1 = std::make_shared<ServerEntry>(cfg3);
        s = slave1->startup(cfg3);
        INVARIANT(s.ok());

        auto single = std::make_shared<ServerEntry>(cfg4);
        s = single->startup(cfg4);
        INVARIANT(s.ok());

        {
            runCmd(slave, { "slaveof", "127.0.0.1", std::to_string(master_port) });
            // slaveof need about 3 seconds to transfer file.
            std::this_thread::sleep_for(std::chrono::seconds(5));

            runCmd(slave1, { "slaveof", "127.0.0.1", std::to_string(slave_port) });
            std::this_thread::sleep_for(std::chrono::seconds(5));

            runCmd(master, { "set", "a", "1" });
            runCmd(master, { "set", "a", "2" });

            runCmd(single, { "set", "a", "1" });
            runCmd(single, { "set", "a", "2" });
        }
        // need wait enough time.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        {
            runCmd(master, { "binlogflush", "all" });
            runCmd(slave, { "binlogflush", "all" });
            runCmd(slave1, { "binlogflush", "all" });
            runCmd(single, { "binlogflush", "all" });
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
        checkBinlogFile(master_dir, false, i);
        checkBinlogFile(slave_dir, true, i);
        checkBinlogFile(slave1_dir, true, i);
        checkBinlogFile(single_dir, true, i);

#ifndef _WIN32
        master->stop();
        slave->stop();
        slave1->stop();
        single->stop();

        ASSERT_EQ(slave.use_count(), 1);
#endif

        LOG(INFO) << ">>>>>> test store count:" << i << " end;";
    }
}

TEST(Repl, SlaveCantModify) {
    size_t i = 1;
    {
        LOG(INFO) << ">>>>>> test store count:" << i;
        const auto guard = MakeGuard([] {
            destroyEnv(master_dir);
            destroyEnv(slave_dir);
            destroyEnv(slave1_dir);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        });

        EXPECT_TRUE(setupEnv(master_dir));
        EXPECT_TRUE(setupEnv(slave_dir));
        EXPECT_TRUE(setupEnv(slave1_dir));

        auto cfg1 = makeServerParam(master_port, i, master_dir, false);
        auto cfg2 = makeServerParam(slave_port, i, slave_dir, false);
        auto cfg3 = makeServerParam(slave1_port, i, slave1_dir, false);

        auto master = std::make_shared<ServerEntry>(cfg1);
        auto s = master->startup(cfg1);
        INVARIANT(s.ok());

        auto slave = std::make_shared<ServerEntry>(cfg2);
        s = slave->startup(cfg2);
        INVARIANT(s.ok());

        auto slave1 = std::make_shared<ServerEntry>(cfg3);
        s = slave1->startup(cfg3);
        INVARIANT(s.ok());

        {
            runCmd(slave, { "slaveof", "127.0.0.1", std::to_string(master_port) });
            // slaveof need about 3 seconds to transfer file.
            std::this_thread::sleep_for(std::chrono::seconds(5));

            runCmd(slave1, { "slaveof", "127.0.0.1", std::to_string(slave_port) });
            std::this_thread::sleep_for(std::chrono::seconds(5));

            auto ctx1 = std::make_shared<asio::io_context>();
            auto session1 = makeSession(master, ctx1);
            session1->setArgs({ "set", "a", "2" });
            auto expect = Command::runSessionCmd(session1.get());
            EXPECT_TRUE(expect.ok());

            waitSlaveCatchup(master, slave);
            waitSlaveCatchup(slave, slave1);

            auto ctx2 = std::make_shared<asio::io_context>();
            auto session2 = makeSession(slave, ctx2);
            session2->setArgs({ "set", "a", "3" });
            expect = Command::runSessionCmd(session2.get());
            EXPECT_EQ(expect.status().toString(), "-ERR:3,msg:txn is replOnly\r\n");

            session2->setArgs({ "get", "a"});
            expect = Command::runSessionCmd(session2.get());
            EXPECT_EQ(expect.value(), "$1\r\n2\r\n");

            auto ctx3 = std::make_shared<asio::io_context>();
            auto session3 = makeSession(slave1, ctx3);
            session3->setArgs({ "set", "a", "4" });
            expect = Command::runSessionCmd(session3.get());
            EXPECT_EQ(expect.status().toString(), "-ERR:3,msg:txn is replOnly\r\n");

            session3->setArgs({ "get", "a"});
            expect = Command::runSessionCmd(session3.get());
            EXPECT_EQ(expect.value(), "$1\r\n2\r\n");
        }

#ifndef _WIN32
        master->stop();
        slave->stop();
        slave1->stop();

        ASSERT_EQ(slave.use_count(), 1);
#endif

        LOG(INFO) << ">>>>>> test store count:" << i << " end;";
    }
}

TEST(Repl, slaveofBenchmarkingMaster) {
    size_t i = 0;
    {
        LOG(INFO) << ">>>>>> test store count:" << i;
        const auto guard = MakeGuard([] {
            destroyEnv(master_dir);
            destroyEnv(slave_dir);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        });

        EXPECT_TRUE(setupEnv(master_dir));
        EXPECT_TRUE(setupEnv(slave_dir));

        auto cfg1 = makeServerParam(master_port, i, master_dir, false);
        auto cfg2 = makeServerParam(slave_port, i, slave_dir, false);

        auto master = std::make_shared<ServerEntry>(cfg1);
        auto s = master->startup(cfg1);
        INVARIANT(s.ok());

        auto slave = std::make_shared<ServerEntry>(cfg2);
        s = slave->startup(cfg2);
        INVARIANT(s.ok());

        LOG(INFO) << ">>>>>> master add data begin.";
        auto thread = std::thread([this, master](){
            testAll(master); // need about 40 seconds
        });
        uint32_t sleep_time = genRand()%20 + 10; // 10-30 seconds
        sleep(sleep_time);

        LOG(INFO) << ">>>>>> slaveof begin.";
        runCmd(slave, { "slaveof", "127.0.0.1", std::to_string(master_port) });
        // slaveof need about 3 seconds to transfer file.
        std::this_thread::sleep_for(std::chrono::seconds(5));

        thread.join();
        waitSlaveCatchup(master, slave);
        sleep(3); // wait recycle binlog
        compareData(master, slave);
        LOG(INFO) << ">>>>>> compareData end.";
#ifndef _WIN32
        master->stop();
        slave->stop();
        ASSERT_EQ(slave.use_count(), 1);
#endif

        LOG(INFO) << ">>>>>> test store count:" << i << " end;";
    }
}

void checkBinlogKeepNum(std::shared_ptr<ServerEntry> svr, uint32_t num) {
    auto ctx = std::make_shared<asio::io_context>();
    auto session = makeSession(svr, ctx);
    for (size_t i = 0; i < svr->getKVStoreCount(); i++) {
        session->setArgs({ "binlogpos", to_string(i)});
        auto expect = Command::runSessionCmd(session.get());
        uint64_t binlogpos = Command::getInt64FromFmtLongLong(expect.value()).value();

        session->setArgs({ "binlogstart", to_string(i)});
        expect = Command::runSessionCmd(session.get());
        uint64_t binlogstart = Command::getInt64FromFmtLongLong(expect.value()).value();
        LOG(INFO) << "checkBinlogKeepNum, port:" << svr->getParams()->port
            << " store:" << i
            << " binlogpos:" << binlogpos
            << " binlogstart:" << binlogstart
            << " num:" << num;
        // if data not full, binlogpos-binlogstart may be smaller than num.
        if(svr->getParams()->binlogDelRange == 1 || svr->getParams()->binlogDelRange == 0){
            EXPECT_TRUE(binlogpos - binlogstart + 1 == num);
        } else {
            EXPECT_TRUE(binlogpos - binlogstart + 1 < num + svr->getParams()->binlogDelRange);
        }
        
    }
}

TEST(Repl, BinlogKeepNum_Test) {
    size_t i = 0;
    {
        for(int j = 0; j < 2; j++){
            LOG(INFO) << ">>>>>> test store count:" << i;
            const auto guard = MakeGuard([] {
                destroyEnv(master_dir);
                destroyEnv(slave_dir);
                destroyEnv(slave1_dir);
                destroyEnv(single_dir);
                std::this_thread::sleep_for(std::chrono::seconds(5));
            });

            EXPECT_TRUE(setupEnv(master_dir));
            EXPECT_TRUE(setupEnv(slave_dir));
            EXPECT_TRUE(setupEnv(slave1_dir));
            EXPECT_TRUE(setupEnv(single_dir));

            auto cfg1 = makeServerParam(master_port, i, master_dir, false);
            auto cfg2 = makeServerParam(slave_port, i, slave_dir, false);
            auto cfg3 = makeServerParam(slave1_port, i, slave1_dir, false);
            auto cfg4 = makeServerParam(single_port, i, single_dir, false);
            uint64_t masterBinlogNum = 10;
            cfg1->maxBinlogKeepNum = masterBinlogNum;
            cfg2->maxBinlogKeepNum = masterBinlogNum;
            cfg2->slaveBinlogKeepNum = 1;
            cfg3->slaveBinlogKeepNum = 1;
            cfg4->maxBinlogKeepNum = masterBinlogNum;
            if(j == 1) {
                cfg1->binlogDelRange = 5000;
                cfg2->binlogDelRange = 5000;
                cfg3->binlogDelRange = 5000;
                cfg4->binlogDelRange = 5000;
            }

            auto master = std::make_shared<ServerEntry>(cfg1);
            auto s = master->startup(cfg1);
            INVARIANT(s.ok());

            auto slave = std::make_shared<ServerEntry>(cfg2);
            s = slave->startup(cfg2);
            INVARIANT(s.ok());

            auto slave1 = std::make_shared<ServerEntry>(cfg3);
            s = slave1->startup(cfg3);
            INVARIANT(s.ok());

            auto single = std::make_shared<ServerEntry>(cfg4);
            s = single->startup(cfg4);
            INVARIANT(s.ok());

            runCmd(slave, { "slaveof", "127.0.0.1", std::to_string(master_port) });
            // slaveof need about 3 seconds to transfer file.
            std::this_thread::sleep_for(std::chrono::seconds(5));

            auto allKeys = initData(master, recordSize);
            initData(single, recordSize);

            waitSlaveCatchup(master, slave);
            sleep(3); // wait recycle binlog

            LOG(INFO) << ">>>>>> checkBinlogKeepNum begin.";
            checkBinlogKeepNum(master, masterBinlogNum);
            checkBinlogKeepNum(slave, 1);
            checkBinlogKeepNum(single, masterBinlogNum);


            LOG(INFO) << ">>>>>> master add data begin.";
            auto thread = std::thread([this, master](){
                testAll(master); // need about 40 seconds
            });
            uint32_t sleep_time = genRand()%20 + 10; // 10-30 seconds
            sleep(sleep_time);

            LOG(INFO) << ">>>>>> slaveof begin.";
            runCmd(slave1, { "slaveof", "127.0.0.1", std::to_string(slave_port) });
            // slaveof need about 3 seconds to transfer file.
            std::this_thread::sleep_for(std::chrono::seconds(5));

            thread.join();
            initData(master, recordSize); // add data every store
            waitSlaveCatchup(master, slave);
            waitSlaveCatchup(slave, slave1);
            sleep(3); // wait recycle binlog
            LOG(INFO) << ">>>>>> checkBinlogKeepNum begin.";
            checkBinlogKeepNum(master, masterBinlogNum);
            checkBinlogKeepNum(slave, masterBinlogNum);
            checkBinlogKeepNum(slave1, 1);
            LOG(INFO) << ">>>>>> checkBinlogKeepNum end.";

    #ifndef _WIN32
            master->stop();
            slave->stop();
            slave1->stop();
            single->stop();

            ASSERT_EQ(slave.use_count(), 1);
            ASSERT_EQ(master.use_count(), 1);
            ASSERT_EQ(slave1.use_count(), 1);
            ASSERT_EQ(single.use_count(), 1);
    #endif

            LOG(INFO) << ">>>>>> test store count:" << i << " end;";   
        }
    }
}

Status scan(const std::string& logfile) {
    FILE* pf = fopen(logfile.c_str(), "r");
    if (pf == NULL) {
        return {ErrorCodes::ERR_INTERNAL, "fopen failed:" + logfile};
    }
    const uint32_t buff_len = 4096;
    char buff[buff_len];
    uint64_t id = 0;

    int ret = fread(buff, BINLOG_HEADER_V2_LEN, 1, pf);
    if (ret != 1 || strstr(buff, BINLOG_HEADER_V2) != buff) {
        cerr << "read head failed." << endl;
        return {ErrorCodes::ERR_INTERNAL, "read head failed."};
    }
    buff[BINLOG_HEADER_V2_LEN] = '\0';
    uint32_t storeId =
        be32toh(*reinterpret_cast<uint32_t*>(buff + strlen(BINLOG_HEADER_V2)));
    EXPECT_TRUE(storeId == 0);

    while (!feof(pf)) {
        // keylen
        uint32_t keylen = 0;
        ret = fread(buff, sizeof(uint32_t), 1, pf);
        if (ret != 1) {
            if (feof(pf)) {
                // cerr << "read logfile end." << endl;
                return {ErrorCodes::ERR_OK, ""}; // read file end.
            }
            return {ErrorCodes::ERR_INTERNAL, "read keylen failed."};
        }
        buff[sizeof(uint32_t)] = '\0';
        keylen = int32Decode(buff);

        // key
        string key;
        key.resize(keylen);
        ret = fread(const_cast<char*>(key.c_str()), keylen, 1, pf);
        if (ret != 1) {
            return {ErrorCodes::ERR_INTERNAL, "read key failed."};
        }

        // valuelen
        uint32_t valuelen = 0;
        ret = fread(buff, sizeof(uint32_t), 1, pf);
        if (ret != 1) {
            return {ErrorCodes::ERR_INTERNAL, "read valuelen failed."};
        }
        buff[sizeof(uint32_t)] = '\0';
        valuelen = int32Decode(buff);

        // value
        string value;
        value.resize(valuelen);
        ret = fread(const_cast<char*>(value.c_str()),
            valuelen, 1, pf);
        if (ret != 1) {
            return {ErrorCodes::ERR_INTERNAL, "read value failed."};
        }

        Expected<ReplLogKeyV2> logkey = ReplLogKeyV2::decode(key);
        if (!logkey.ok()) {
            return {ErrorCodes::ERR_INTERNAL, "decode logkey failed."};
        }

        Expected<ReplLogValueV2> logValue = ReplLogValueV2::decode(value);
        if (!logValue.ok()) {
            return {ErrorCodes::ERR_INTERNAL, "decode logvalue failed."};
        }

        if(id == 0) {
            id = logkey.value().getBinlogId() + 1;
            continue;
        }
        if(id != logkey.value().getBinlogId()) {
            return {ErrorCodes::ERR_INTERNAL, "binlogId error."};
        }
        id++;
    }

    fclose(pf);
    return {ErrorCodes::ERR_OK, ""};
}

TEST(Repl, coreDumpWhenSaveBinlog) {
    size_t i = 0;
    {
        LOG(INFO) << ">>>>>> test store count:" << i;
        const auto guard = MakeGuard([] {
            destroyEnv(single_dir);
            std::this_thread::sleep_for(std::chrono::seconds(5));
        });

        EXPECT_TRUE(setupEnv(single_dir));

        auto cfg = makeServerParam(single_port, i, single_dir, false);
        uint64_t masterBinlogNum = 10;
        cfg->maxBinlogKeepNum = masterBinlogNum;
        cfg->binlogDelRange = 5000;

        {
            auto single = std::make_shared<ServerEntry>(cfg);
            auto s = single->startup(cfg);
            INVARIANT(s.ok());

            initData(single, recordSize);

            single->stop();

            LOG(INFO) << ">>>>>> scanDumpFile begin.";
            std::string subpath = "./repltest_single/dump/0/";
            try {
                for (auto& p : filesystem::recursive_directory_iterator(subpath)) {
                    const filesystem::path& path = p.path();
                    if (!filesystem::is_regular_file(p)) {
                        LOG(INFO) << "maxDumpFileSeq ignore:" << p.path();
                        continue;
                    }
                    // assert path with dir prefix
                    INVARIANT(path.string().find(subpath) == 0);
                    std::string relative = path.string().erase(0, subpath.size());
                    if (relative.substr(0, 6) != "binlog") {
                        LOG(INFO) << "maxDumpFileSeq ignore:" << relative;
                        continue;
                    }
                    auto s = scan(path.string());
                    EXPECT_TRUE(s.ok());
                }
            } catch (const std::exception& ex) {
                LOG(ERROR) <<" get fileno failed:" << ex.what();
                EXPECT_TRUE(0);
                return;
            }
        }
        sleep(3);
        
        auto single = std::make_shared<ServerEntry>(cfg);
        auto s = single->startup(cfg);
        INVARIANT(s.ok());

        sleep(3);

        LOG(INFO) << ">>>>>> checkBinlogKeepNum begin.";
        checkBinlogKeepNum(single, masterBinlogNum);

        LOG(INFO) << ">>>>>> scanDumpFile begin.";
        std::string subpath = "./repltest_single/dump/0/";
        try {
            for (auto& p : filesystem::recursive_directory_iterator(subpath)) {
                const filesystem::path& path = p.path();
                if (!filesystem::is_regular_file(p)) {
                    LOG(INFO) << "maxDumpFileSeq ignore:" << p.path();
                    continue;
                }
                // assert path with dir prefix
                INVARIANT(path.string().find(subpath) == 0);
                std::string relative = path.string().erase(0, subpath.size());
                if (relative.substr(0, 6) != "binlog") {
                    LOG(INFO) << "maxDumpFileSeq ignore:" << relative;
                    continue;
                }
                auto s = scan(path.string());
                if(!s.ok()) {
                    LOG(ERROR) << "scan failed:"<< s.toString();
                }
                EXPECT_TRUE(s.ok());
            }
        } catch (const std::exception& ex) {
            LOG(ERROR) <<" get fileno failed:" << ex.what();
            EXPECT_TRUE(0);
            return;
        }

#ifndef _WIN32
        single->stop();

        ASSERT_EQ(single.use_count(), 1);
#endif

        LOG(INFO) << ">>>>>> test store count:" << i << " end;";   
    }
}

}  // namespace tendisplus