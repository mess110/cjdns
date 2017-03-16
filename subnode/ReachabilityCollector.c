/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "dht/dhtcore/ReplySerializer.h"
#include "subnode/ReachabilityCollector.h"
#include "util/log/Log.h"
#include "util/Identity.h"
#include "util/events/Timeout.h"
#include "util/AddrTools.h"

#define TIMEOUT_MILLISECONDS 10000

struct PeerInfo
{
    // Address of the peer from us
    struct Address addr;

    // Their path to us
    uint64_t pathThemToUs;

    // Next path to check when sending getPeers requests to our peer looking for ourselves.
    uint64_t pathToCheck;

    bool querying;

    struct Allocator* alloc;

    Identity
};

#define ArrayList_NAME OfPeerInfo
#define ArrayList_TYPE struct PeerInfo
#include "util/ArrayList.h"

struct ReachabilityCollector_pvt
{
    struct ReachabilityCollector pub;
    struct MsgCore* msgCore;
    struct Allocator* alloc;
    struct Log* log;
    struct BoilerplateResponder* br;
    struct Address* myAddr;

    struct ArrayList_OfPeerInfo* piList;

    Identity
};

static void mkNextRequest(struct ReachabilityCollector_pvt* rcp);

static void change0(struct ReachabilityCollector_pvt* rcp,
                    struct Address* nodeAddr,
                    struct Allocator* tempAlloc)
{
    for (int i = 0; i < rcp->piList->length; i++) {
        struct PeerInfo* pi = ArrayList_OfPeerInfo_get(rcp->piList, i);
        if (Address_isSameIp(nodeAddr, &pi->addr)) {
            if (nodeAddr->path == 0) {
                Log_debug(rcp->log, "Peer [%s] dropped",
                    Address_toString(&pi->addr, tempAlloc)->bytes);
                ArrayList_OfPeerInfo_remove(rcp->piList, i);
                Allocator_free(pi->alloc);
            } else if (nodeAddr->path != pi->addr.path) {
                Log_debug(rcp->log, "Peer [%s] changed path",
                    Address_toString(&pi->addr, tempAlloc)->bytes);
                pi->pathThemToUs = -1;
                pi->pathToCheck = 1;
                pi->querying = true;
            }
            return;
        }
    }
    if (nodeAddr->path == 0) {
        Log_debug(rcp->log, "Nonexistant peer [%s] dropped",
            Address_toString(nodeAddr, tempAlloc)->bytes);
        return;
    }
    struct Allocator* piAlloc = Allocator_child(rcp->alloc);
    struct PeerInfo* pi = Allocator_calloc(piAlloc, sizeof(struct PeerInfo), 1);
    Identity_set(pi);
    Bits_memcpy(&pi->addr, nodeAddr, Address_SIZE);
    pi->alloc = piAlloc;
    pi->querying = true;
    pi->pathToCheck = 1;
    pi->pathThemToUs = -1;
    ArrayList_OfPeerInfo_add(rcp->piList, pi);
    Log_debug(rcp->log, "Peer [%s] added", Address_toString(&pi->addr, tempAlloc)->bytes);
    mkNextRequest(rcp);
}

void ReachabilityCollector_change(struct ReachabilityCollector* rc, struct Address* nodeAddr)
{
    struct ReachabilityCollector_pvt* rcp = Identity_check((struct ReachabilityCollector_pvt*) rc);
    struct Allocator* tempAlloc = Allocator_child(rcp->alloc);
    change0(rcp, nodeAddr, tempAlloc);
    Allocator_free(tempAlloc);
}

static void onReplyTimeout(struct MsgCore_Promise* prom)
{
    // meh do nothing, we'll just ping it again...
}

static void onReply(Dict* msg, struct Address* src, struct MsgCore_Promise* prom)
{
    struct ReachabilityCollector_pvt* rcp =
        Identity_check((struct ReachabilityCollector_pvt*) prom->userData);
    if (!src) {
        onReplyTimeout(prom);
        mkNextRequest(rcp);
        return;
    }
    struct Address_List* results = ReplySerializer_parse(src, msg, rcp->log, false, prom->alloc);
    uint64_t path = 1;

    struct PeerInfo* pi = NULL;
    for (int j = 0; j < rcp->piList->length; j++) {
        struct PeerInfo* pi0 = ArrayList_OfPeerInfo_get(rcp->piList, j);
        if (Address_isSameIp(&pi0->addr, src)) {
            pi = pi0;
            break;
        }
    }
    if (!pi) {
        Log_debug(rcp->log, "Got message from peer which is gone from list");
        return;
    }

    for (int i = results->length - 1; i >= 0; i--) {
        path = results->elems[i].path;
        Log_debug(rcp->log, "getPeers result [%s]",
            Address_toString(&results->elems[i], prom->alloc)->bytes);
        if (Bits_memcmp(results->elems[i].ip6.bytes, rcp->myAddr->ip6.bytes, 16)) { continue; }
        if (pi->pathThemToUs != path) {
            Log_debug(rcp->log, "Found back-route for [%s]",
                Address_toString(src, prom->alloc)->bytes);
            pi->pathThemToUs = path;
            if (rcp->pub.onChange) {
                rcp->pub.onChange(
                    &rcp->pub, src->ip6.bytes, path, src->path, 0, 0xffff, 0xffff, 0xffff);
            }
        }
        pi->querying = false;
        mkNextRequest(rcp);
        return;
    }
    pi->pathToCheck = (results->length < 8) ? 1 : path;
    mkNextRequest(rcp);
}

static void mkNextRequest(struct ReachabilityCollector_pvt* rcp)
{
    struct PeerInfo* pi = NULL;
    for (int i = 0; i < rcp->piList->length; i++) {
        pi = ArrayList_OfPeerInfo_get(rcp->piList, i);
        if (pi->querying) { break; }
    }
    if (!pi || !pi->querying) {
        Log_debug(rcp->log, "All [%u] peers have been queried", rcp->piList->length);
        return;
    }
    struct MsgCore_Promise* query =
        MsgCore_createQuery(rcp->msgCore, TIMEOUT_MILLISECONDS, rcp->alloc);
    query->userData = rcp;
    query->cb = onReply;
    query->target = Address_clone(&pi->addr, query->alloc);
    Dict* d = query->msg = Dict_new(query->alloc);
    Dict_putStringCC(d, "q", "gp", query->alloc);
    uint64_t label_be = Endian_hostToBigEndian64(pi->pathToCheck);

    uint8_t targetPath[20];
    AddrTools_printPath(targetPath, pi->pathToCheck);
    Log_debug(rcp->log, "Getting peers for peer [%s] tar [%s]",
        Address_toString(&pi->addr, query->alloc)->bytes,
        targetPath);

    Dict_putStringC(d, "tar",
        String_newBinary((uint8_t*) &label_be, 8, query->alloc), query->alloc);
    BoilerplateResponder_addBoilerplate(rcp->br, d, &pi->addr, query->alloc);
}

static void cycle(void* vrc)
{
    struct ReachabilityCollector_pvt* rcp = Identity_check((struct ReachabilityCollector_pvt*) vrc);
    mkNextRequest(rcp);
}

struct ReachabilityCollector* ReachabilityCollector_new(struct Allocator* allocator,
                                                        struct MsgCore* msgCore,
                                                        struct Log* log,
                                                        struct EventBase* base,
                                                        struct BoilerplateResponder* br,
                                                        struct Address* myAddr)
{
    struct Allocator* alloc = Allocator_child(allocator);
    struct ReachabilityCollector_pvt* rcp =
        Allocator_calloc(alloc, sizeof(struct ReachabilityCollector_pvt), 1);
    rcp->myAddr = myAddr;
    rcp->msgCore = msgCore;
    rcp->alloc = alloc;
    rcp->piList = ArrayList_OfPeerInfo_new(alloc);
    rcp->log = log;
    rcp->br = br;
    Identity_set(rcp);
    Timeout_setInterval(cycle, rcp, 2000, base, alloc);
    return &rcp->pub;
}