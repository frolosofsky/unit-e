// Copyright (c) 2012-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>

#include <consensus/validation.h>
#include <rpc/server.h>
#include <test/test_unite.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

extern UniValue importmulti(const JSONRPCRequest& request);
extern UniValue dumpwallet(const JSONRPCRequest& request);
extern UniValue importwallet(const JSONRPCRequest& request);

// how many times to run all the tests to have a chance to catch errors that only show up with particular random shuffles
#define RUN_TESTS 100

// some tests fail 1% of the time due to bad luck.
// we repeat those tests this many times and only complain if all iterations of the test fail
#define RANDOM_REPEATS 5

std::vector<std::unique_ptr<CWalletTx>> wtxn;

typedef std::set<CInputCoin> CoinSet;

BOOST_FIXTURE_TEST_SUITE(wallet_tests, WalletTestingSetup)

static const CWallet testWallet;
static std::vector<COutput> vCoins;

static void add_coin(const CAmount& nValue, int nAge = 6*24, bool fIsFromMe = false, int nInput=0)
{
    static int nextLockTime = 0;
    CMutableTransaction tx;
    tx.nLockTime = nextLockTime++;        // so all transactions get different hashes
    tx.vout.resize(nInput+1);
    tx.vout[nInput].nValue = nValue;
    if (fIsFromMe) {
        // IsFromMe() returns (GetDebit() > 0), and GetDebit() is 0 if vin.empty(),
        // so stop vin being empty, and cache a non-zero Debit to fake out IsFromMe()
        tx.vin.resize(1);
    }
    std::unique_ptr<CWalletTx> wtx(new CWalletTx(&testWallet, MakeTransactionRef(std::move(tx))));
    if (fIsFromMe)
    {
        wtx->fDebitCached = true;
        wtx->nDebitCached = 1;
    }
    COutput output(wtx.get(), nInput, nAge, true /* spendable */, true /* solvable */, true /* safe */);
    vCoins.push_back(output);
    wtxn.emplace_back(std::move(wtx));
}

static void empty_wallet(void)
{
    vCoins.clear();
    wtxn.clear();
}

static bool equal_sets(CoinSet a, CoinSet b)
{
    std::pair<CoinSet::iterator, CoinSet::iterator> ret = mismatch(a.begin(), a.end(), b.begin());
    return ret.first == a.end() && ret.second == b.end();
}

BOOST_AUTO_TEST_CASE(coin_selection_tests)
{
    CoinSet setCoinsRet, setCoinsRet2;
    CAmount nValueRet;

    LOCK(testWallet.cs_wallet);

    // test multiple times to allow for differences in the shuffle order
    for (int i = 0; i < RUN_TESTS; i++)
    {
        empty_wallet();

        // with an empty wallet we can't even pay one cent
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 1 * EEES, 1, 6, 0, vCoins, setCoinsRet, nValueRet));

        add_coin(1*EEES, 4);        // add a new 1 cent coin

        // with a new 1 cent coin, we still can't find a mature 1 cent
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 1 * EEES, 1, 6, 0, vCoins, setCoinsRet, nValueRet));

        // but we can find a new 1 cent
        BOOST_CHECK( testWallet.SelectCoinsMinConf( 1 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * EEES);

        add_coin(2*EEES);           // add a mature 2 cent coin

        // we can't make 3 cents of mature coins
        BOOST_CHECK(!testWallet.SelectCoinsMinConf( 3 * EEES, 1, 6, 0, vCoins, setCoinsRet, nValueRet));

        // we can make 3 cents of new  coins
        BOOST_CHECK( testWallet.SelectCoinsMinConf( 3 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 3 * EEES);

        add_coin(5*EEES);           // add a mature 5 cent coin,
        add_coin(10*EEES, 3, true); // a new 10 cent coin sent from one of our own addresses
        add_coin(20*EEES);          // and a mature 20 cent coin

        // now we have new: 1+10=11 (of which 10 was self-sent), and mature: 2+5+20=27.  total = 38

        // we can't make 38 cents only if we disallow new coins:
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(38 * EEES, 1, 6, 0, vCoins, setCoinsRet, nValueRet));
        // we can't even make 37 cents if we don't allow new coins even if they're from us
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(38 * EEES, 6, 6, 0, vCoins, setCoinsRet, nValueRet));
        // but we can make 37 cents if we accept new coins from ourself
        BOOST_CHECK( testWallet.SelectCoinsMinConf(37 * EEES, 1, 6, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 37 * EEES);
        // and we can make 38 cents if we accept all new coins
        BOOST_CHECK( testWallet.SelectCoinsMinConf(38 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 38 * EEES);

        // try making 34 cents from 1,2,5,10,20 - we can't do it exactly
        BOOST_CHECK( testWallet.SelectCoinsMinConf(34 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 35 * EEES);       // but 35 cents is closest
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);     // the best should be 20+10+5.  it's incredibly unlikely the 1 or 2 got included (but possible)

        // when we try making 7 cents, the smaller coins (1,2,5) are enough.  We should see just 2+5
        BOOST_CHECK( testWallet.SelectCoinsMinConf( 7 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 7 * EEES);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // when we try making 8 cents, the smaller coins (1,2,5) are exactly enough.
        BOOST_CHECK( testWallet.SelectCoinsMinConf( 8 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK(nValueRet == 8 * EEES);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        // when we try making 9 cents, no subset of smaller coins is enough, and we get the next bigger coin (10)
        BOOST_CHECK( testWallet.SelectCoinsMinConf( 9 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 10 * EEES);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // now clear out the wallet and start again to test choosing between subsets of smaller coins and the next biggest coin
        empty_wallet();

        add_coin( 6*EEES);
        add_coin( 7*EEES);
        add_coin( 8*EEES);
        add_coin(20*EEES);
        add_coin(30*EEES); // now we have 6+7+8+20+30 = 71 cents total

        // check that we have 71 and not 72
        BOOST_CHECK( testWallet.SelectCoinsMinConf(71 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK(!testWallet.SelectCoinsMinConf(72 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));

        // now try making 16 cents.  the best smaller coins can do is 6+7+8 = 21; not as good at the next biggest coin, 20
        BOOST_CHECK( testWallet.SelectCoinsMinConf(16 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 20 * EEES); // we should get 20 in one coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        add_coin( 5*EEES); // now we have 5+6+7+8+20+30 = 75 cents total

        // now if we try making 16 cents again, the smaller coins can make 5+6+7 = 18 cents, better than the next biggest coin, 20
        BOOST_CHECK( testWallet.SelectCoinsMinConf(16 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 18 * EEES); // we should get 18 in 3 coins
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        add_coin( 18*EEES); // now we have 5+6+7+8+18+20+30

        // and now if we try making 16 cents again, the smaller coins can make 5+6+7 = 18 cents, the same as the next biggest coin, 18
        BOOST_CHECK( testWallet.SelectCoinsMinConf(16 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 18 * EEES);  // we should get 18 in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U); // because in the event of a tie, the biggest coin wins

        // now try making 11 cents.  we should get 5+6
        BOOST_CHECK( testWallet.SelectCoinsMinConf(11 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 11 * EEES);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // check that the smallest bigger coin is used
        add_coin( 1*UNIT);
        add_coin( 2*UNIT);
        add_coin( 3*UNIT);
        add_coin( 4*UNIT); // now we have 5+6+7+8+18+20+30+100+200+300+400 = 1094 cents
        BOOST_CHECK( testWallet.SelectCoinsMinConf(95 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * UNIT);  // we should get 1 UTE in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        BOOST_CHECK( testWallet.SelectCoinsMinConf(195 * EEES, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 2 * UNIT);  // we should get 2 UTE in 1 coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // empty the wallet and start again, now with fractions of a cent, to test small change avoidance

        empty_wallet();
        add_coin(MIN_CHANGE * 1 / 10);
        add_coin(MIN_CHANGE * 2 / 10);
        add_coin(MIN_CHANGE * 3 / 10);
        add_coin(MIN_CHANGE * 4 / 10);
        add_coin(MIN_CHANGE * 5 / 10);

        // try making 1 * MIN_CHANGE from the 1.5 * MIN_CHANGE
        // we'll get change smaller than MIN_CHANGE whatever happens, so can expect MIN_CHANGE exactly
        BOOST_CHECK( testWallet.SelectCoinsMinConf(MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, MIN_CHANGE);

        // but if we add a bigger coin, small change is avoided
        add_coin(1111*MIN_CHANGE);

        // try making 1 from 0.1 + 0.2 + 0.3 + 0.4 + 0.5 + 1111 = 1112.5
        BOOST_CHECK( testWallet.SelectCoinsMinConf(1 * MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * MIN_CHANGE); // we should get the exact amount

        // if we add more small coins:
        add_coin(MIN_CHANGE * 6 / 10);
        add_coin(MIN_CHANGE * 7 / 10);

        // and try again to make 1.0 * MIN_CHANGE
        BOOST_CHECK( testWallet.SelectCoinsMinConf(1 * MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1 * MIN_CHANGE); // we should get the exact amount

        // run the 'mtgox' test (see http://blockexplorer.com/tx/29a3efd3ef04f9153d47a990bd7b048a4b2d213daaa5fb8ed670fb85f13bdbcf)
        // they tried to consolidate 10 50k coins into one 500k coin, and ended up with 50k in change
        empty_wallet();
        for (int j = 0; j < 20; j++)
            add_coin(50000 * UNIT);

        BOOST_CHECK( testWallet.SelectCoinsMinConf(500000 * UNIT, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 500000 * UNIT); // we should get the exact amount
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 10U); // in ten coins

        // if there's not enough in the smaller coins to make at least 1 * MIN_CHANGE change (0.5+0.6+0.7 < 1.0+1.0),
        // we need to try finding an exact subset anyway

        // sometimes it will fail, and so we use the next biggest coin:
        empty_wallet();
        add_coin(MIN_CHANGE * 5 / 10);
        add_coin(MIN_CHANGE * 6 / 10);
        add_coin(MIN_CHANGE * 7 / 10);
        add_coin(1111 * MIN_CHANGE);
        BOOST_CHECK( testWallet.SelectCoinsMinConf(1 * MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 1111 * MIN_CHANGE); // we get the bigger coin
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);

        // but sometimes it's possible, and we use an exact subset (0.4 + 0.6 = 1.0)
        empty_wallet();
        add_coin(MIN_CHANGE * 4 / 10);
        add_coin(MIN_CHANGE * 6 / 10);
        add_coin(MIN_CHANGE * 8 / 10);
        add_coin(1111 * MIN_CHANGE);
        BOOST_CHECK( testWallet.SelectCoinsMinConf(MIN_CHANGE, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, MIN_CHANGE);   // we should get the exact amount
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U); // in two coins 0.4+0.6

        // test avoiding small change
        empty_wallet();
        add_coin(MIN_CHANGE * 5 / 100);
        add_coin(MIN_CHANGE * 1);
        add_coin(MIN_CHANGE * 100);

        // trying to make 100.01 from these three coins
        BOOST_CHECK(testWallet.SelectCoinsMinConf(MIN_CHANGE * 10001 / 100, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, MIN_CHANGE * 10105 / 100); // we should get all coins
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 3U);

        // but if we try to make 99.9, we should take the bigger of the two small coins to avoid small change
        BOOST_CHECK(testWallet.SelectCoinsMinConf(MIN_CHANGE * 9990 / 100, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
        BOOST_CHECK_EQUAL(nValueRet, 101 * MIN_CHANGE);
        BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

        // test with many inputs
        for (CAmount amt=1500; amt < UNIT; amt*=10) {
             empty_wallet();
             // Create 676 inputs (=  (old MAX_STANDARD_TX_SIZE == 100000)  / 148 bytes per input)
             for (uint16_t j = 0; j < 676; j++)
                 add_coin(amt);
             BOOST_CHECK(testWallet.SelectCoinsMinConf(2000, 1, 1, 0, vCoins, setCoinsRet, nValueRet));
             if (amt - 2000 < MIN_CHANGE) {
                 // needs more than one input:
                 uint16_t returnSize = std::ceil((2000.0 + MIN_CHANGE)/amt);
                 CAmount returnValue = amt * returnSize;
                 BOOST_CHECK_EQUAL(nValueRet, returnValue);
                 BOOST_CHECK_EQUAL(setCoinsRet.size(), returnSize);
             } else {
                 // one input is sufficient:
                 BOOST_CHECK_EQUAL(nValueRet, amt);
                 BOOST_CHECK_EQUAL(setCoinsRet.size(), 1U);
             }
        }

        // test randomness
        {
            empty_wallet();
            for (int i2 = 0; i2 < 100; i2++)
                add_coin(UNIT);

            // picking 50 from 100 coins doesn't depend on the shuffle,
            // but does depend on randomness in the stochastic approximation code
            BOOST_CHECK(testWallet.SelectCoinsMinConf(50 * UNIT, 1, 6, 0, vCoins, setCoinsRet , nValueRet));
            BOOST_CHECK(testWallet.SelectCoinsMinConf(50 * UNIT, 1, 6, 0, vCoins, setCoinsRet2, nValueRet));
            BOOST_CHECK(!equal_sets(setCoinsRet, setCoinsRet2));

            int fails = 0;
            for (int j = 0; j < RANDOM_REPEATS; j++)
            {
                // selecting 1 from 100 identical coins depends on the shuffle; this test will fail 1% of the time
                // run the test RANDOM_REPEATS times and only complain if all of them fail
                BOOST_CHECK(testWallet.SelectCoinsMinConf(UNIT, 1, 6, 0, vCoins, setCoinsRet , nValueRet));
                BOOST_CHECK(testWallet.SelectCoinsMinConf(UNIT, 1, 6, 0, vCoins, setCoinsRet2, nValueRet));
                if (equal_sets(setCoinsRet, setCoinsRet2))
                    fails++;
            }
            BOOST_CHECK_NE(fails, RANDOM_REPEATS);

            // add 75 cents in small change.  not enough to make 90 cents,
            // then try making 90 cents.  there are multiple competing "smallest bigger" coins,
            // one of which should be picked at random
            add_coin(5 * EEES);
            add_coin(10 * EEES);
            add_coin(15 * EEES);
            add_coin(20 * EEES);
            add_coin(25 * EEES);

            fails = 0;
            for (int j = 0; j < RANDOM_REPEATS; j++)
            {
                // selecting 1 from 100 identical coins depends on the shuffle; this test will fail 1% of the time
                // run the test RANDOM_REPEATS times and only complain if all of them fail
                BOOST_CHECK(testWallet.SelectCoinsMinConf(90*EEES, 1, 6, 0, vCoins, setCoinsRet , nValueRet));
                BOOST_CHECK(testWallet.SelectCoinsMinConf(90*EEES, 1, 6, 0, vCoins, setCoinsRet2, nValueRet));
                if (equal_sets(setCoinsRet, setCoinsRet2))
                    fails++;
            }
            BOOST_CHECK_NE(fails, RANDOM_REPEATS);
        }
    }
    empty_wallet();
}

BOOST_AUTO_TEST_CASE(ApproximateBestSubset)
{
    CoinSet setCoinsRet;
    CAmount nValueRet;

    LOCK(testWallet.cs_wallet);

    empty_wallet();

    // Test vValue sort order
    for (int i = 0; i < 1000; i++)
        add_coin(1000 * UNIT);
    add_coin(3 * UNIT);

    BOOST_CHECK(testWallet.SelectCoinsMinConf(1003 * UNIT, 1, 6, 0, vCoins, setCoinsRet, nValueRet));
    BOOST_CHECK_EQUAL(nValueRet, 1003 * UNIT);
    BOOST_CHECK_EQUAL(setCoinsRet.size(), 2U);

    empty_wallet();
}

static void AddKey(CWallet& wallet, const CKey& key)
{
    LOCK(wallet.cs_wallet);
    wallet.AddKeyPubKey(key, key.GetPubKey());
}

BOOST_FIXTURE_TEST_CASE(rescan, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    CBlockIndex* const nullBlock = nullptr;
    CBlockIndex* oldTip = chainActive.Tip();
    GetBlockFileInfo(oldTip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE;
    CTransactionRef new_coinbase = CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0];
    CBlockIndex* newTip = chainActive.Tip();

    LOCK(cs_main);

    // Verify ScanForWalletTransactions picks up transactions in both the old
    // and new block files.
    {
        CWallet wallet;
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(&wallet);
        reserver.reserve();
        BOOST_CHECK_EQUAL(nullBlock, wallet.ScanForWalletTransactions(oldTip, nullptr, reserver));
        BOOST_CHECK_EQUAL(wallet.GetImmatureBalance(), coinbaseTxns.back().vout[0].nValue + new_coinbase->vout[0].nValue);
    }

    // Prune the older block file.
    PruneOneBlockFile(oldTip->GetBlockPos().nFile);
    UnlinkPrunedFiles({oldTip->GetBlockPos().nFile});

    // Verify ScanForWalletTransactions only picks transactions in the new block
    // file.
    {
        CWallet wallet;
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(&wallet);
        reserver.reserve();
        BOOST_CHECK_EQUAL(oldTip, wallet.ScanForWalletTransactions(oldTip, nullptr, reserver));
        BOOST_CHECK_EQUAL(wallet.GetImmatureBalance(), coinbaseTxns.back().vout[0].nValue);
    }

    // Verify importmulti RPC returns failure for a key whose creation time is
    // before the missing block, and success for a key whose creation time is
    // after.
    {
        CWallet wallet;
        vpwallets.clear(); // Remove the wallet used to create the chain
        vpwallets.insert(vpwallets.begin(), &wallet);
        UniValue keys;
        keys.setArray();
        UniValue key;
        key.setObject();
        key.pushKV("scriptPubKey", HexStr(GetScriptForRawPubKey(coinbaseKey.GetPubKey())));
        key.pushKV("timestamp", 0);
        key.pushKV("internal", UniValue(true));
        keys.push_back(key);
        key.clear();
        key.setObject();
        CKey futureKey;
        futureKey.MakeNewKey(true);
        key.pushKV("scriptPubKey", HexStr(GetScriptForRawPubKey(futureKey.GetPubKey())));
        key.pushKV("timestamp", newTip->GetBlockTimeMax() + TIMESTAMP_WINDOW + 1);
        key.pushKV("internal", UniValue(true));
        keys.push_back(key);
        JSONRPCRequest request;
        request.params.setArray();
        request.params.push_back(keys);

        UniValue response = importmulti(request);
        BOOST_CHECK_EQUAL(response.write(),
            strprintf("[{\"success\":false,\"error\":{\"code\":-1,\"message\":\"Rescan failed for key with creation "
                      "timestamp %d. There was an error reading a block from time %d, which is after or within %d "
                      "seconds of key creation, and could contain transactions pertaining to the key. As a result, "
                      "transactions and coins using this key may not appear in the wallet. This error could be caused "
                      "by pruning or data corruption (see unit-e log for details) and could be dealt with by "
                      "downloading and rescanning the relevant blocks (see -reindex and -rescan "
                      "options).\"}},{\"success\":true}]",
                              0, oldTip->GetBlockTimeMax(), TIMESTAMP_WINDOW));
        vpwallets.erase(vpwallets.begin());
    }
}

// Verify importwallet RPC starts rescan at earliest block with timestamp
// greater or equal than key birthday. Previously there was a bug where
// importwallet RPC would start the scan at the latest block with timestamp less
// than or equal to key birthday.
BOOST_FIXTURE_TEST_CASE(importwallet_rescan, TestChain100Setup)
{
    g_address_type = OUTPUT_TYPE_DEFAULT;
    g_change_type = OUTPUT_TYPE_DEFAULT;

    // Create two blocks with same timestamp to verify that importwallet rescan
    // will pick up both blocks, not just the first.
    const int64_t BLOCK_TIME = chainActive.Tip()->GetBlockTimeMax() + 5;
    SetMockTime(BLOCK_TIME);
    coinbaseTxns.emplace_back(*CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    coinbaseTxns.emplace_back(*CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);

    // Set key birthday to block time increased by the timestamp window, so
    // rescan will start at the block time.
    const int64_t KEY_TIME = BLOCK_TIME + TIMESTAMP_WINDOW;
    SetMockTime(KEY_TIME);
    coinbaseTxns.emplace_back(*CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);

    LOCK(cs_main);
    vpwallets.clear(); // Remove the wallet used to create the chain
    // Import key into wallet and call dumpwallet to create backup file.
    {
        CWallet wallet;
        LOCK(wallet.cs_wallet);
        wallet.mapKeyMetadata[coinbaseKey.GetPubKey().GetID()].nCreateTime = KEY_TIME;
        wallet.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());

        JSONRPCRequest request;
        request.params.setArray();
        request.params.push_back((pathTemp / "wallet.backup").string());
        vpwallets.insert(vpwallets.begin(), &wallet);
        ::dumpwallet(request);
    }

    // Call importwallet RPC and verify all blocks with timestamps >= BLOCK_TIME
    // were scanned, and no prior blocks were scanned.
    {
        CWallet wallet;

        JSONRPCRequest request;
        request.params.setArray();
        request.params.push_back((pathTemp / "wallet.backup").string());
        vpwallets[0] = &wallet;
        ::importwallet(request);

        LOCK(wallet.cs_wallet);
        BOOST_CHECK_EQUAL(wallet.mapWallet.size(), 3);
        BOOST_CHECK_EQUAL(coinbaseTxns.size(), 103);
        for (size_t i = 0; i < coinbaseTxns.size(); ++i) {
            bool found = wallet.GetWalletTx(coinbaseTxns[i].GetHash());
            bool expected = i >= 100;
            BOOST_CHECK_EQUAL(found, expected);
        }
    }

    SetMockTime(0);
    vpwallets.erase(vpwallets.begin());
}

// Check that GetImmatureCredit() returns a newly calculated value instead of
// the cached value after a MarkDirty() call.
//
// This is a regression test written to verify a bugfix for the immature credit
// function. Similar tests probably should be written for the other credit and
// debit functions.
BOOST_FIXTURE_TEST_CASE(coin_mark_dirty_immature_credit, TestChain100Setup)
{
    CWallet wallet;
    CWalletTx wtx(&wallet, MakeTransactionRef(coinbaseTxns.back()));
    LOCK2(cs_main, wallet.cs_wallet);
    wtx.hashBlock = chainActive.Tip()->GetBlockHash();
    wtx.nIndex = 0;

    // Call GetImmatureCredit() once before adding the key to the wallet to
    // cache the current immature credit amount, which is 0.
    BOOST_CHECK_EQUAL(wtx.GetImmatureCredit(), 0);

    // Invalidate the cached value, add the key, and make sure a new immature
    // credit amount is calculated.
    wtx.MarkDirty();
    wallet.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());
    BOOST_CHECK_EQUAL(wtx.GetImmatureCredit(), wtx.tx->vout[0].nValue);
}

BOOST_FIXTURE_TEST_CASE(get_immature_credit, TestChain100Setup)
{
  // Make the first coinbase mature
  CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
  {
    LOCK(cs_main);
    const CWalletTx *const immature_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.back().GetHash());
    BOOST_CHECK_EQUAL(immature_coinbase->GetImmatureCredit(), immature_coinbase->tx->vout[0].nValue);

    const CWalletTx *const mature_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.front().GetHash());
    BOOST_CHECK_EQUAL(mature_coinbase->GetImmatureCredit(), 0);
  }

  // Make the second coinbase mature
  CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

  {
    LOCK(cs_main);
    const CWalletTx *const immature_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.at(2).GetHash());
    BOOST_CHECK_EQUAL(immature_coinbase->GetImmatureCredit(), immature_coinbase->tx->vout[0].nValue);

    const CWalletTx *const mature_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.at(1).GetHash());
    BOOST_CHECK_EQUAL(mature_coinbase->GetImmatureCredit(), 0);
  }
}

BOOST_FIXTURE_TEST_CASE(get_available_credit, TestChain100Setup)
{
  // Make the first coinbase mature
  CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
  {
    LOCK(cs_main);
    const CWalletTx *const immature_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.back().GetHash());
    BOOST_CHECK_EQUAL(immature_coinbase->GetAvailableCredit(), 0);

    const CWalletTx *const mature_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.front().GetHash());
    BOOST_CHECK_EQUAL(mature_coinbase->GetAvailableCredit(), mature_coinbase->tx->vout[0].nValue);
  }

  // Make the second coinbase mature
  CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

  {
    LOCK(cs_main);
    const CWalletTx *const immature_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.at(2).GetHash());
    BOOST_CHECK_EQUAL(immature_coinbase->GetAvailableCredit(), 0);

    const CWalletTx *const mature_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.at(1).GetHash());
    BOOST_CHECK_EQUAL(mature_coinbase->GetAvailableCredit(), immature_coinbase->tx->vout[0].nValue);
  }
}

BOOST_FIXTURE_TEST_CASE(get_immature_watch_only_credit, TestChain100Setup)
{
  CKey watch_only_key;
  watch_only_key.MakeNewKey(true);
  const CScript watch_only_script = GetScriptForRawPubKey(watch_only_key.GetPubKey());
  {
    LOCK(pwalletMain->cs_wallet);
    assert(pwalletMain->AddWatchOnly(watch_only_script, 0));
  }

  CTransactionRef immature_coinbase = CreateAndProcessBlock({}, watch_only_script).vtx[0];

  {
     LOCK(cs_main);
     const CWalletTx *const wallet_tx = pwalletMain->GetWalletTx(immature_coinbase->GetHash());
     BOOST_CHECK_EQUAL(wallet_tx->GetImmatureWatchOnlyCredit(), immature_coinbase->vout[0].nValue);
  }

  // Make the coinbase watch-only mature
  for (int i = 0; i < COINBASE_MATURITY; ++i) {
    CreateAndProcessBlock({}, GetScriptForRawPubKey(watch_only_key.GetPubKey()));
  }

  {
    LOCK(cs_main);
    const CWalletTx *const wallet_tx = pwalletMain->GetWalletTx(immature_coinbase->GetHash());
    BOOST_CHECK_EQUAL(wallet_tx->GetImmatureWatchOnlyCredit(), 0);
  }
}

BOOST_FIXTURE_TEST_CASE(get_available_watch_only_credit, TestChain100Setup)
{
  CKey watch_only_key;
  watch_only_key.MakeNewKey(true);
  const CScript watch_only_script = GetScriptForRawPubKey(watch_only_key.GetPubKey());
  {
    LOCK(pwalletMain->cs_wallet);
    assert(pwalletMain->AddWatchOnly(watch_only_script, 0));
  }

  CTransactionRef watch_only_coinbase = CreateAndProcessBlock({}, watch_only_script).vtx[0];

  {
    LOCK(cs_main);
    const CWalletTx *wallet_tx = pwalletMain->GetWalletTx(watch_only_coinbase->GetHash());
    // The stake is watch-only
    BOOST_CHECK_EQUAL(wallet_tx->GetAvailableWatchOnlyCredit(), 10000 * UNIT);
  }

  // Make the coinbase watch-only mature mining using the rewards just made mature
  for (int i = 0; i < COINBASE_MATURITY; ++i) {
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
  }

  {
    // The initial stake of 10000 * UNIT also beacame watch-only cause we proposed with a watch-only script
    LOCK(cs_main);
    const CWalletTx *wallet_tx = pwalletMain->GetWalletTx(watch_only_coinbase->GetHash());
    BOOST_CHECK_EQUAL(wallet_tx->GetAvailableWatchOnlyCredit(), watch_only_coinbase->GetValueOut());
  }
}

static int64_t AddTx(CWallet& wallet, uint32_t lockTime, int64_t mockTime, int64_t blockTime)
{
    CMutableTransaction tx;
    tx.nLockTime = lockTime;
    SetMockTime(mockTime);
    CBlockIndex* block = nullptr;
    if (blockTime > 0) {
        LOCK(cs_main);
        const auto inserted = mapBlockIndex.emplace(GetRandHash(), new CBlockIndex);
        assert(inserted.second);
        const uint256& hash = inserted.first->first;
        block = inserted.first->second;
        block->nTime = blockTime;
        block->phashBlock = &hash;
    }

    CWalletTx wtx(&wallet, MakeTransactionRef(tx));
    if (block) {
        wtx.SetMerkleBranch(block, 0);
    }
    wallet.AddToWallet(wtx);
    LOCK(wallet.cs_wallet);
    return wallet.mapWallet.at(wtx.GetHash()).nTimeSmart;
}

// Simple test to verify assignment of CWalletTx::nSmartTime value. Could be
// expanded to cover more corner cases of smart time logic.
BOOST_AUTO_TEST_CASE(ComputeTimeSmart)
{
    CWallet wallet;

    // New transaction should use clock time if lower than block time.
    BOOST_CHECK_EQUAL(AddTx(wallet, 1, 100, 120), 100);

    // Test that updating existing transaction does not change smart time.
    BOOST_CHECK_EQUAL(AddTx(wallet, 1, 200, 220), 100);

    // New transaction should use clock time if there's no block time.
    BOOST_CHECK_EQUAL(AddTx(wallet, 2, 300, 0), 300);

    // New transaction should use block time if lower than clock time.
    BOOST_CHECK_EQUAL(AddTx(wallet, 3, 420, 400), 400);

    // New transaction should use latest entry time if higher than
    // min(block time, clock time).
    BOOST_CHECK_EQUAL(AddTx(wallet, 4, 500, 390), 400);

    // If there are future entries, new transaction should use time of the
    // newest entry that is no more than 300 seconds ahead of the clock time.
    BOOST_CHECK_EQUAL(AddTx(wallet, 5, 50, 600), 300);

    // Reset mock time for other tests.
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(LoadReceiveRequests)
{
    CTxDestination dest = CKeyID();
    LOCK(pwalletMain->cs_wallet);
    pwalletMain->AddDestData(dest, "misc", "val_misc");
    pwalletMain->AddDestData(dest, "rr0", "val_rr0");
    pwalletMain->AddDestData(dest, "rr1", "val_rr1");

    const std::vector<std::string> values = pwalletMain->GetDestValues("rr");
    BOOST_CHECK_EQUAL(values.size(), 2);
    BOOST_CHECK_EQUAL(values[0], "val_rr0");
    BOOST_CHECK_EQUAL(values[1], "val_rr1");
}

class ListCoinsTestingSetup : public TestChain100Setup
{
public:
    ListCoinsTestingSetup()
    {
        CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));
    }

    CWalletTx& AddTx(CRecipient recipient)
    {
        CWalletTx wtx;
        CReserveKey reservekey(pwalletMain.get());
        CAmount fee;
        int changePos = -1;
        std::string error;
        CCoinControl dummy;
        BOOST_CHECK(pwalletMain->CreateTransaction({recipient}, wtx, reservekey, fee, changePos, error, dummy));
        CValidationState state;
        BOOST_CHECK(pwalletMain->CommitTransaction(wtx, reservekey, nullptr, state));
        CMutableTransaction blocktx;
        {
            LOCK(pwalletMain->cs_wallet);
            blocktx = CMutableTransaction(*pwalletMain->mapWallet.at(wtx.GetHash()).tx);
        }
        CreateAndProcessBlock({CMutableTransaction(blocktx)}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));
        LOCK(pwalletMain->cs_wallet);
        auto it = pwalletMain->mapWallet.find(wtx.GetHash());
        BOOST_CHECK(it != pwalletMain->mapWallet.end());
        it->second.SetMerkleBranch(chainActive.Tip(), 1);
        return it->second;
    }
};

BOOST_FIXTURE_TEST_CASE(ListCoins, ListCoinsTestingSetup)
{
    std::string coinbaseAddress = coinbaseKey.GetPubKey().GetID().ToString();

    // Confirm ListCoins initially returns 2 coins grouped under coinbaseKey
    // address.
    std::map<CTxDestination, std::vector<COutput>> list = pwalletMain->ListCoins();
    BOOST_CHECK_EQUAL(list.size(), 1);
    BOOST_CHECK_EQUAL(list.begin()->first.which(), 4);
    BOOST_CHECK_EQUAL(boost::get<WitnessV0KeyHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 2); // Mature reward + inital stake

    // Check initial balance from one mature coinbase transaction + the initial funds.
    BOOST_CHECK_EQUAL(10000 * UNIT + coinbaseTxns.back().vout[0].nValue, pwalletMain->GetAvailableBalance());

    // Make another block reward mature so we can spend it for a transaction
    CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));

    // Add a transaction creating a change address, and confirm ListCoins still
    // returns the coins associated with the change address underneath the
    // coinbaseKey pubkey, even though the change address has a different
    // pubkey.
    AddTx(CRecipient{GetScriptForDestination(WitnessV0KeyHash()), 1 * UNIT, false /* subtract fee */});
    list = pwalletMain->ListCoins();
    BOOST_CHECK_EQUAL(list.size(), 1);
    BOOST_CHECK_EQUAL(list.begin()->first.which(), 4);
    BOOST_CHECK_EQUAL(boost::get<WitnessV0KeyHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 4); // stake + change + 2 mature rewards

    // Lock all coins. Confirm number of available coins drops to 0.
    std::vector<COutput> available;
    pwalletMain->AvailableCoins(available);
    BOOST_CHECK_EQUAL(available.size(), 4);
    for (const auto& group : list) {
        for (const auto& coin : group.second) {
          LOCK(pwalletMain->cs_wallet);
          pwalletMain->LockCoin(COutPoint(coin.tx->GetHash(), coin.i));
        }
    }
    pwalletMain->AvailableCoins(available);
    BOOST_CHECK_EQUAL(available.size(), 0);

    // Confirm ListCoins still returns same result as before, despite coins
    // being locked.
    list = pwalletMain->ListCoins();
    BOOST_CHECK_EQUAL(list.size(), 1);
    BOOST_CHECK_EQUAL(list.begin()->first.which(), 4);
    BOOST_CHECK_EQUAL(boost::get<WitnessV0KeyHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 4);
}

BOOST_FIXTURE_TEST_CASE(AvailableCoins_coinbase_maturity, TestChain100Setup) {

  std::vector<COutput> stake_available;
  pwalletMain->AvailableCoins(stake_available);
  BOOST_CHECK_EQUAL(stake_available.size(), 1);
  BOOST_CHECK_EQUAL(stake_available[0].tx->tx->vout[stake_available[0].i].nValue, 10000 * UNIT);

  // Make one coinbase mature
  CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

  std::vector<COutput> available;
  pwalletMain->AvailableCoins(available);
  // Stake + block reward are now available
  BOOST_CHECK_EQUAL(available.size(), 2);
}

// Test that AvailableCoins follows coin control settings for
// ignoring remotely staked coins.
BOOST_FIXTURE_TEST_CASE(AvailableCoins, ListCoinsTestingSetup)
{
    std::vector<COutput> coins;

    CKey our_key;
    CKey our_second_key;
    our_key.MakeNewKey(/* compressed: */ true);
    our_second_key.MakeNewKey(/* compressed: */ true);
    CScript witness_script = GetScriptForMultisig(1, {our_key.GetPubKey(), our_second_key.GetPubKey()});
    {
        LOCK(pwalletMain->cs_wallet);
        pwalletMain->AddKey(our_key);
        pwalletMain->AddKey(our_second_key);
        pwalletMain->AddCScript(witness_script);
    }

    CKey their_key;
    their_key.MakeNewKey(true);

    pwalletMain->AvailableCoins(coins);
    // One coinbase has reached maturity + the stake
    BOOST_CHECK_EQUAL(2, coins.size());

    AddTx(CRecipient{
        CScript::CreateRemoteStakingKeyhashScript(
            ToByteVector(their_key.GetPubKey().GetID()),
            ToByteVector(our_key.GetPubKey().GetSha256())),
        1 * UNIT, false
    });

    AddTx(CRecipient{
        CScript::CreateRemoteStakingScripthashScript(
            ToByteVector(their_key.GetPubKey().GetID()),
            ToByteVector(Sha256(witness_script.begin(), witness_script.end()))),
        1 * UNIT, false
    });

    pwalletMain->AvailableCoins(coins);
    // Two coinbase and one remote staking output and the initial stake
    BOOST_CHECK_EQUAL(6, coins.size());

    CCoinControl coin_control;
    coin_control.m_ignore_remote_staked = true;

    pwalletMain->AvailableCoins(coins, true, &coin_control);
    // Remote staking output should be ignored
    BOOST_CHECK_EQUAL(4, coins.size());
}

BOOST_FIXTURE_TEST_CASE(GetAddressBalances_coinbase_maturity, TestChain100Setup) {

  {
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const std::map<CTxDestination, CAmount> balances = pwalletMain->GetAddressBalances();
    BOOST_CHECK_EQUAL(balances.size(), 1); // the stake
  }

  // Make one coinbase mature
  CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

  {
    const CTxDestination coinbase_destination = GetDestinationForKey(coinbaseKey.GetPubKey(), OUTPUT_TYPE_LEGACY);
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const std::map<CTxDestination, CAmount> balances = pwalletMain->GetAddressBalances();
    BOOST_CHECK_EQUAL(balances.size(), 2);
    BOOST_CHECK_EQUAL(balances.at(coinbase_destination), 10000 * UNIT);
  }
}

BOOST_FIXTURE_TEST_CASE(GetLegacyBalance_coinbase_maturity, TestChain100Setup) {

  // Nothing is mature currenly so no balances (except the inital stake)
  {
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const CAmount all_balance = pwalletMain->GetLegacyBalance(ISMINE_ALL, 0, nullptr);
    const CAmount spendable_balance = pwalletMain->GetLegacyBalance(ISMINE_SPENDABLE, 0, nullptr);
    const CAmount watchonly_balance = pwalletMain->GetLegacyBalance(ISMINE_WATCH_ONLY, 0, nullptr);
    BOOST_CHECK_EQUAL(all_balance, 10000 * UNIT);
    BOOST_CHECK_EQUAL(spendable_balance, 10000 * UNIT);
    BOOST_CHECK_EQUAL(watchonly_balance, 0);
  }

  // Make one coinbase mature
  CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

  // Now we should have the same balance as before plus the newly mature coinbase
  {
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const CAmount all_balance = pwalletMain->GetLegacyBalance(ISMINE_ALL, 0, nullptr);
    const CAmount spendable_balance = pwalletMain->GetLegacyBalance(ISMINE_SPENDABLE, 0, nullptr);
    const CAmount watchonly_balance = pwalletMain->GetLegacyBalance(ISMINE_WATCH_ONLY, 0, nullptr);
    BOOST_CHECK_EQUAL(all_balance, (10000 * UNIT) + coinbaseTxns.front().vout[0].nValue);
    BOOST_CHECK_EQUAL(spendable_balance, (10000 * UNIT) + coinbaseTxns.back().vout[0].nValue);
    BOOST_CHECK_EQUAL(watchonly_balance, 0);
  }

  // Now add a new watch-only key, craete a new coinbase and then make it mature
  CKey watch_only_key;
  watch_only_key.MakeNewKey(true);
  const CScript watch_only_script = GetScriptForRawPubKey(watch_only_key.GetPubKey());

  {
    LOCK(pwalletMain->cs_wallet);
    assert(pwalletMain->AddWatchOnly(watch_only_script, 0));
  }

  // Make one more coinbase mature so we can use it to mine after we spent our
  // last output for creating the watch-only block
  CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));

  auto watch_only_coinbase = CreateAndProcessBlock({}, GetScriptForRawPubKey(watch_only_key.GetPubKey())).vtx[0];

  for (int i = 0; i < COINBASE_MATURITY + 1; ++i) {
    CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));
  }

  // As per mature outputs we should have 103 blocks worth of rewards
  // - 1 reward used to stake the watch-only + the initial stake + the
  // watch-only stake and reward
  {
      auto coinbase_reward = coinbaseTxns.back().vout[0].nValue;
      LOCK2(cs_main, pwalletMain->cs_wallet);
      const CAmount all_balance = pwalletMain->GetLegacyBalance(ISMINE_ALL, 0, nullptr);
      const CAmount spendable_balance = pwalletMain->GetLegacyBalance(ISMINE_SPENDABLE, 0, nullptr);
      const CAmount watchonly_balance = pwalletMain->GetLegacyBalance(ISMINE_WATCH_ONLY, 0, nullptr);
      BOOST_CHECK_EQUAL(all_balance, (10000 * UNIT) + coinbase_reward * 102 + watch_only_coinbase->GetValueOut());
      BOOST_CHECK_EQUAL(spendable_balance, (10000 * UNIT) + coinbase_reward * 102);
      BOOST_CHECK_EQUAL(watchonly_balance, watch_only_coinbase->GetValueOut());
  }
}

BOOST_FIXTURE_TEST_CASE(GetBlockToMaturity, TestChain100Setup)
{
  // Make the first coinbase mature
  CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

  const blockchain::Height height = chainActive.Height();
  {
    LOCK(cs_main);
    const CWalletTx *const first = pwalletMain->GetWalletTx(coinbaseTxns.front().GetHash());
    BOOST_CHECK(first);
    // Height is 101, COINBASE_MATURITY is 100, so we expect the coinbase to be mature
    BOOST_CHECK_EQUAL(first->GetBlocksToRewardMaturity(), 0);

    const CWalletTx *const next_to_first = pwalletMain->GetWalletTx(coinbaseTxns.at(1).GetHash());
    BOOST_CHECK(next_to_first);
    BOOST_CHECK_EQUAL(next_to_first->GetBlocksToRewardMaturity(), 1);

    const CWalletTx *const middle = pwalletMain->GetWalletTx(coinbaseTxns.at(coinbaseTxns.size()/2).GetHash());
    BOOST_CHECK(middle);
    BOOST_CHECK_EQUAL(middle->GetBlocksToRewardMaturity(), COINBASE_MATURITY - (height/2));

    // Just another block has been created on top of the last coibase, so we expect
    // it to need other COINBASE_MATURITY - 1 confirmations
    const CWalletTx *const last = pwalletMain->GetWalletTx(coinbaseTxns.back().GetHash());
    BOOST_CHECK(last);
    BOOST_CHECK_EQUAL(last->GetBlocksToRewardMaturity(), COINBASE_MATURITY - 1);
  }

  // Create 10 more blocks
  CBlock last_block;
  for (int i = 0; i < 10; ++i) {
    last_block = CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
  }

  {
    LOCK(cs_main);
    CWalletTx last_generated_coinbase(pwalletMain.get(), last_block.vtx[0]);
    BOOST_CHECK_EQUAL(last_generated_coinbase.GetBlocksToRewardMaturity(), COINBASE_MATURITY + 1);

    const CWalletTx *const last_coinbase = pwalletMain->GetWalletTx(coinbaseTxns.back().GetHash());
    BOOST_CHECK(last_coinbase);
    BOOST_CHECK_EQUAL(last_coinbase->GetBlocksToRewardMaturity(), COINBASE_MATURITY - 11);
  }
}

BOOST_FIXTURE_TEST_CASE(GetCredit_coinbase_maturity, TestChain100Setup) {

  // Nothing is mature currenly so no balances (except the initial stake)
  {
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const CWalletTx *const first = pwalletMain->GetWalletTx(coinbaseTxns.front().GetHash());
    const CAmount all_credit = first->GetCredit(ISMINE_ALL);
    const CAmount spendable_credit = first->GetCredit(ISMINE_SPENDABLE);
    const CAmount watchonly_credit = first->GetCredit(ISMINE_WATCH_ONLY);
    BOOST_CHECK_EQUAL(all_credit, 10000 * UNIT);
    BOOST_CHECK_EQUAL(spendable_credit, 10000 * UNIT);
    BOOST_CHECK_EQUAL(watchonly_credit, 0);
  }

  // Make one coinbase mature
  CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));

  {
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const CWalletTx *const first = pwalletMain->GetWalletTx(coinbaseTxns.front().GetHash());
    const CAmount all_credit = first->GetCredit(ISMINE_ALL);
    const CAmount spendable_credit = first->GetCredit(ISMINE_SPENDABLE);
    const CAmount watchonly_credit = first->GetCredit(ISMINE_WATCH_ONLY);
    BOOST_CHECK_EQUAL(all_credit, coinbaseTxns.back().GetValueOut());
    BOOST_CHECK_EQUAL(spendable_credit, coinbaseTxns.back().GetValueOut());
    BOOST_CHECK_EQUAL(watchonly_credit, 0);
  }

  // Now add a new watch-only key, create a new coinbase and then make it mature
  CKey watch_only_key;
  watch_only_key.MakeNewKey(true);
  const CScript watch_only_script = GetScriptForRawPubKey(watch_only_key.GetPubKey());

  {
    LOCK(pwalletMain->cs_wallet);
    assert(pwalletMain->AddWatchOnly(watch_only_script, 0));
  }

  // Make one more coinbase mature so we can use it to mine after we spent our
  // last output for creating the watch-only block
  CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));

  CTransactionRef watch_only_coinbase = CreateAndProcessBlock({}, GetScriptForRawPubKey(watch_only_key.GetPubKey())).vtx[0];

  for (int i = 0; i < COINBASE_MATURITY; ++i) {
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
  }

  {
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const CWalletTx *const watch_only = pwalletMain->GetWalletTx(watch_only_coinbase->GetHash());
    const CAmount all_credit = watch_only->GetCredit(ISMINE_ALL);
    const CAmount spendable_credit = watch_only->GetCredit(ISMINE_SPENDABLE);
    const CAmount watchonly_credit = watch_only->GetCredit(ISMINE_WATCH_ONLY);
    BOOST_CHECK_EQUAL(all_credit, watch_only_coinbase->GetValueOut());
    BOOST_CHECK_EQUAL(spendable_credit, 0);
    BOOST_CHECK_EQUAL(watchonly_credit, watch_only_coinbase->GetValueOut());
  }
}

BOOST_FIXTURE_TEST_CASE(GetCredit_coinbase_cache, TestChain100Setup) {
  // Nothing is mature (except the initial stake) currenlty so nothing should be cached
  {
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const CWalletTx *const first = pwalletMain->GetWalletTx(coinbaseTxns.front().GetHash());
    const CAmount available_credit = first->GetAvailableCredit(true);
    const CAmount all_credit = first->GetCredit(ISMINE_ALL);
    BOOST_CHECK_EQUAL(all_credit, 10000 * UNIT);
    BOOST_CHECK_EQUAL(first->fCreditCached, false);
    BOOST_CHECK_EQUAL(first->nCreditCached, 0);
    BOOST_CHECK_EQUAL(first->fAvailableCreditCached, false);
    BOOST_CHECK_EQUAL(first->nAvailableCreditCached, 0);
    BOOST_CHECK_EQUAL(available_credit, 0);
  }

  // Make one coinbase mature
  CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));
  {
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const CWalletTx *const first = pwalletMain->GetWalletTx(coinbaseTxns.front().GetHash());

    // Since we didn't call GetBalance or GetAvailableCredit yet, nothing should be cached
    BOOST_CHECK_EQUAL(first->fCreditCached, false);
    BOOST_CHECK_EQUAL(first->nCreditCached, 0);
    BOOST_CHECK_EQUAL(first->fAvailableCreditCached, false);
    BOOST_CHECK_EQUAL(first->nAvailableCreditCached, 0);

    // The available credit is just the mature reward because the stake has been
    // already spent at this point
    const CAmount all_credit = first->GetCredit(ISMINE_ALL);
    const CAmount available_credit = first->GetAvailableCredit(true);
    BOOST_CHECK_EQUAL(all_credit, (10000 * UNIT) + coinbaseTxns.front().vout[0].nValue);
    BOOST_CHECK_EQUAL(available_credit, coinbaseTxns.front().vout[0].nValue);
    BOOST_CHECK_EQUAL(first->fCreditCached, true);
    BOOST_CHECK_EQUAL(first->nCreditCached, (10000 * UNIT) + coinbaseTxns.front().vout[0].nValue);
    BOOST_CHECK_EQUAL(first->fAvailableCreditCached, true);
    BOOST_CHECK_EQUAL(first->nAvailableCreditCached, coinbaseTxns.front().vout[0].nValue);

    // Calling the second time should result in the same (cached) values
    BOOST_CHECK_EQUAL(all_credit, first->GetCredit(ISMINE_ALL));
    BOOST_CHECK_EQUAL(available_credit, first->GetAvailableCredit(true));

    // Change the cached values to verify that they are the ones used
    first->nCreditCached = all_credit - 5 * UNIT;
    first->nAvailableCreditCached = available_credit - 7 * UNIT;
    BOOST_CHECK_EQUAL(all_credit - 5 * UNIT, first->GetCredit(ISMINE_ALL));
    BOOST_CHECK_EQUAL(available_credit - 7 * UNIT, first->GetAvailableCredit(true));

    // Verify that the amounts will be recalculated properly
    first->fCreditCached = false;
    first->fAvailableCreditCached = false;
    BOOST_CHECK_EQUAL(all_credit, first->GetCredit(ISMINE_ALL));
    BOOST_CHECK_EQUAL(available_credit, first->GetAvailableCredit(true));
  }

  // Now add a new watch-only key, create a new coinbase and then make it mature
  CKey watch_only_key;
  watch_only_key.MakeNewKey(true);
  const CScript watch_only_script = GetScriptForRawPubKey(watch_only_key.GetPubKey());

  {
    LOCK(pwalletMain->cs_wallet);
    assert(pwalletMain->AddWatchOnly(watch_only_script, 0));
  }

  // The initial stake is gonna be used to generate this block and it will become
  // watch-only
  CTransactionRef watch_only_coinbase = CreateAndProcessBlock({}, GetScriptForRawPubKey(watch_only_key.GetPubKey())).vtx[0];

  for (int i = 0; i < COINBASE_MATURITY + 1; ++i) {
    CreateAndProcessBlock({}, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey().GetID())));
  }

  {
    LOCK2(cs_main, pwalletMain->cs_wallet);

    const CWalletTx *const watch_only = pwalletMain->GetWalletTx(watch_only_coinbase->GetHash());

    BOOST_CHECK_EQUAL(watch_only->fWatchCreditCached, false);
    BOOST_CHECK_EQUAL(watch_only->nWatchCreditCached, 0);
    BOOST_CHECK_EQUAL(watch_only->fAvailableWatchCreditCached, false);
    BOOST_CHECK_EQUAL(watch_only->nAvailableWatchCreditCached, 0);

    const CAmount watch_only_credit = watch_only->GetCredit(ISMINE_WATCH_ONLY);
    const CAmount available_watch_only_credit = watch_only->GetAvailableWatchOnlyCredit(true);

    BOOST_CHECK_EQUAL(watch_only_credit, watch_only_coinbase->GetValueOut());
    BOOST_CHECK_EQUAL(available_watch_only_credit, watch_only_coinbase->GetValueOut());
    BOOST_CHECK_EQUAL(watch_only->fWatchCreditCached, true);
    BOOST_CHECK_EQUAL(watch_only->nWatchCreditCached, watch_only_coinbase->GetValueOut());
    BOOST_CHECK_EQUAL(watch_only->fAvailableWatchCreditCached, true);
    BOOST_CHECK_EQUAL(watch_only->nAvailableWatchCreditCached, watch_only_coinbase->GetValueOut());

    // Calling the second time should result in the same (cached) values
    BOOST_CHECK_EQUAL(watch_only_credit, watch_only->GetCredit(ISMINE_WATCH_ONLY));
    BOOST_CHECK_EQUAL(available_watch_only_credit, watch_only->GetAvailableWatchOnlyCredit(true));

    // Verify cache is used
    watch_only->nWatchCreditCached = watch_only_credit - 1 * UNIT;
    watch_only->nAvailableWatchCreditCached = available_watch_only_credit - 2 * UNIT;
    BOOST_CHECK_EQUAL(watch_only_credit - 1 * UNIT, watch_only->GetCredit(ISMINE_WATCH_ONLY));
    BOOST_CHECK_EQUAL(available_watch_only_credit - 2 * UNIT, watch_only->GetAvailableWatchOnlyCredit(true));

    // Verify that the amounts will be recalculated properly
    watch_only->fWatchCreditCached = false;
    watch_only->fAvailableWatchCreditCached = false;
    BOOST_CHECK_EQUAL(watch_only_credit, watch_only->GetCredit(ISMINE_WATCH_ONLY));
    BOOST_CHECK_EQUAL(available_watch_only_credit, watch_only->GetAvailableWatchOnlyCredit(true));
  }
}

BOOST_AUTO_TEST_SUITE_END()
