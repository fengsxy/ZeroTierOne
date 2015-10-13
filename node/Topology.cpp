/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include "Constants.hpp"
#include "Topology.hpp"
#include "RuntimeEnvironment.hpp"
#include "Dictionary.hpp"
#include "Node.hpp"
#include "Buffer.hpp"

namespace ZeroTier {

// Default World
#define ZT_DEFAULT_WORLD_LENGTH 1
static const unsigned char ZT_DEFAULT_WORLD[ZT_DEFAULT_WORLD_LENGTH] = { 0 };

Topology::Topology(const RuntimeEnvironment *renv) :
	RR(renv),
	_amRoot(false)
{
	try {
		std::string dsWorld(RR->node->dataStoreGet("world"));
		Buffer<ZT_WORLD_MAX_SERIALIZED_LENGTH> dswtmp(dsWorld.data(),dsWorld.length());
		_world.deserialize(dswtmp,0);
	} catch ( ... ) {
		_world = World(); // set to null if cached world is invalid
	}
	{
		World defaultWorld;
		Buffer<ZT_DEFAULT_WORLD_LENGTH> wtmp(ZT_DEFAULT_WORLD,ZT_DEFAULT_WORLD_LENGTH);
		defaultWorld.deserialize(wtmp,0); // throws on error, which would indicate a bad static variable up top
		if (_world.verifyUpdate(defaultWorld)) {
			_world = defaultWorld;
			RR->node->dataStorePut("world",ZT_DEFAULT_WORLD,ZT_DEFAULT_WORLD_LENGTH,false);
		}
	}

	std::string alls(RR->node->dataStoreGet("peers.save"));
	const uint8_t *all = reinterpret_cast<const uint8_t *>(alls.data());
	RR->node->dataStoreDelete("peers.save");

	unsigned int ptr = 0;
	while ((ptr + 4) < alls.size()) {
		// Each Peer serializes itself prefixed by a record length (not including the size of the length itself)
		unsigned int reclen = (unsigned int)all[ptr] & 0xff;
		reclen <<= 8;
		reclen |= (unsigned int)all[ptr + 1] & 0xff;
		reclen <<= 8;
		reclen |= (unsigned int)all[ptr + 2] & 0xff;
		reclen <<= 8;
		reclen |= (unsigned int)all[ptr + 3] & 0xff;

		if (((ptr + reclen) > alls.size())||(reclen > ZT_PEER_SUGGESTED_SERIALIZATION_BUFFER_SIZE))
			break;

		try {
			unsigned int pos = 0;
			SharedPtr<Peer> p(Peer::deserializeNew(RR->identity,Buffer<ZT_PEER_SUGGESTED_SERIALIZATION_BUFFER_SIZE>(all + ptr,reclen),pos));
			if (pos != reclen)
				break;
			ptr += pos;
			if ((p)&&(p->address() != RR->identity.address())) {
				_peers[p->address()] = p;
			} else {
				break; // stop if invalid records
			}
		} catch (std::exception &exc) {
			break;
		} catch ( ... ) {
			break; // stop if invalid records
		}
	}

	clean(RR->node->now());

	for(std::vector<World::Root>::const_iterator r(_world.roots().begin());r!=_world.roots().end();++r) {
		if (r->identity == RR->identity)
			_amRoot = true;
		_rootAddresses.push_back(r->identity.address());
		SharedPtr<Peer> *rp = _peers.get(r->identity.address());
		if (rp) {
			_rootPeers.push_back(*rp);
		} else if (r->identity.address() != RR->identity.address()) {
			SharedPtr<Peer> newrp(new Peer(RR->identity,r->identity));
			_peers.set(r->identity.address(),newrp);
			_rootPeers.push_back(newrp);
		}
	}
}

Topology::~Topology()
{
	Buffer<ZT_PEER_SUGGESTED_SERIALIZATION_BUFFER_SIZE> pbuf;
	std::string all;

	Address *a = (Address *)0;
	SharedPtr<Peer> *p = (SharedPtr<Peer> *)0;
	Hashtable< Address,SharedPtr<Peer> >::Iterator i(_peers);
	while (i.next(a,p)) {
		if (std::find(_rootAddresses.begin(),_rootAddresses.end(),*a) == _rootAddresses.end()) {
			pbuf.clear();
			try {
				(*p)->serialize(pbuf);
				try {
					all.append((const char *)pbuf.data(),pbuf.size());
				} catch ( ... ) {
					return; // out of memory? just skip
				}
			} catch ( ... ) {} // peer too big? shouldn't happen, but it so skip
		}
	}

	RR->node->dataStorePut("peers.save",all,true);
}

SharedPtr<Peer> Topology::addPeer(const SharedPtr<Peer> &peer)
{
	if (peer->address() == RR->identity.address()) {
		TRACE("BUG: addNewPeer() caught and ignored attempt to add peer for self");
		throw std::logic_error("cannot add peer for self");
	}

	const uint64_t now = RR->node->now();
	Mutex::Lock _l(_lock);

	SharedPtr<Peer> &p = _peers.set(peer->address(),peer);
	p->use(now);
	_saveIdentity(p->identity());

	return p;
}

SharedPtr<Peer> Topology::getPeer(const Address &zta)
{
	if (zta == RR->identity.address()) {
		TRACE("BUG: ignored attempt to getPeer() for self, returned NULL");
		return SharedPtr<Peer>();
	}

	const uint64_t now = RR->node->now();
	Mutex::Lock _l(_lock);

	SharedPtr<Peer> &ap = _peers[zta];

	if (ap) {
		ap->use(now);
		return ap;
	}

	Identity id(_getIdentity(zta));
	if (id) {
		try {
			ap = SharedPtr<Peer>(new Peer(RR->identity,id));
			ap->use(now);
			return ap;
		} catch ( ... ) {} // invalid identity?
	}

	_peers.erase(zta);

	return SharedPtr<Peer>();
}

SharedPtr<Peer> Topology::getBestRoot(const Address *avoid,unsigned int avoidCount,bool strictAvoid)
{
	SharedPtr<Peer> bestRoot;
	const uint64_t now = RR->node->now();
	Mutex::Lock _l(_lock);

	if (_amRoot) {
		/* If I am a root server, the "best" root server is the one whose address
		 * is numerically greater than mine (with wrap at top of list). This
		 * causes packets searching for a route to pretty much literally
		 * circumnavigate the globe rather than bouncing between just two. */

		if (_rootAddresses.size() > 1) { // gotta be one other than me for this to work
			std::vector<Address>::const_iterator sna(std::find(_rootAddresses.begin(),_rootAddresses.end(),RR->identity.address()));
			if (sna != _rootAddresses.end()) { // sanity check -- _amRoot should've been false in this case
				for(;;) {
					if (++sna == _rootAddresses.end())
						sna = _rootAddresses.begin(); // wrap around at end
					if (*sna != RR->identity.address()) { // pick one other than us -- starting from me+1 in sorted set order
						SharedPtr<Peer> *p = _peers.get(*sna);
						if ((p)&&((*p)->hasActiveDirectPath(now))) {
							bestRoot = *p;
							break;
						}
					}
				}
			}
		}
	} else {
		/* If I am not a root server, the best root server is the active one with
		 * the lowest latency. */

		unsigned int l,bestLatency = 65536;
		uint64_t lds,ldr;

		// First look for a best root by comparing latencies, but exclude
		// root servers that have not responded to direct messages in order to
		// try to exclude any that are dead or unreachable.
		for(std::vector< SharedPtr<Peer> >::const_iterator sn(_rootPeers.begin());sn!=_rootPeers.end();) {
			// Skip explicitly avoided relays
			for(unsigned int i=0;i<avoidCount;++i) {
				if (avoid[i] == (*sn)->address())
					goto keep_searching_for_roots;
			}

			// Skip possibly comatose or unreachable relays
			lds = (*sn)->lastDirectSend();
			ldr = (*sn)->lastDirectReceive();
			if ((lds)&&(lds > ldr)&&((lds - ldr) > ZT_PEER_RELAY_CONVERSATION_LATENCY_THRESHOLD))
				goto keep_searching_for_roots;

			if ((*sn)->hasActiveDirectPath(now)) {
				l = (*sn)->latency();
				if (bestRoot) {
					if ((l)&&(l < bestLatency)) {
						bestLatency = l;
						bestRoot = *sn;
					}
				} else {
					if (l)
						bestLatency = l;
					bestRoot = *sn;
				}
			}

keep_searching_for_roots:
			++sn;
		}

		if (bestRoot) {
			bestRoot->use(now);
			return bestRoot;
		} else if (strictAvoid)
			return SharedPtr<Peer>();

		// If we have nothing from above, just pick one without avoidance criteria.
		for(std::vector< SharedPtr<Peer> >::const_iterator sn=_rootPeers.begin();sn!=_rootPeers.end();++sn) {
			if ((*sn)->hasActiveDirectPath(now)) {
				unsigned int l = (*sn)->latency();
				if (bestRoot) {
					if ((l)&&(l < bestLatency)) {
						bestLatency = l;
						bestRoot = *sn;
					}
				} else {
					if (l)
						bestLatency = l;
					bestRoot = *sn;
				}
			}
		}
	}

	if (bestRoot)
		bestRoot->use(now);
	return bestRoot;
}

void Topology::clean(uint64_t now)
{
	Mutex::Lock _l(_lock);
	Hashtable< Address,SharedPtr<Peer> >::Iterator i(_peers);
	Address *a = (Address *)0;
	SharedPtr<Peer> *p = (SharedPtr<Peer> *)0;
	while (i.next(a,p)) {
		if (((now - (*p)->lastUsed()) >= ZT_PEER_IN_MEMORY_EXPIRATION)&&(std::find(_rootAddresses.begin(),_rootAddresses.end(),*a) == _rootAddresses.end())) {
			_peers.erase(*a);
		} else {
			(*p)->clean(RR,now);
		}
	}
}

Identity Topology::_getIdentity(const Address &zta)
{
	char p[128];
	Utils::snprintf(p,sizeof(p),"iddb.d/%.10llx",(unsigned long long)zta.toInt());
	std::string ids(RR->node->dataStoreGet(p));
	if (ids.length() > 0) {
		try {
			return Identity(ids);
		} catch ( ... ) {} // ignore invalid IDs
	}
	return Identity();
}

void Topology::_saveIdentity(const Identity &id)
{
	if (id) {
		char p[128];
		Utils::snprintf(p,sizeof(p),"iddb.d/%.10llx",(unsigned long long)id.address().toInt());
		RR->node->dataStorePut(p,id.toString(false),false);
	}
}

} // namespace ZeroTier
