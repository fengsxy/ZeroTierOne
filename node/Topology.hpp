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

#ifndef ZT_TOPOLOGY_HPP
#define ZT_TOPOLOGY_HPP

#include <stdio.h>
#include <string.h>

#include <map>
#include <vector>
#include <stdexcept>
#include <algorithm>

#include "Constants.hpp"

#include "Address.hpp"
#include "Identity.hpp"
#include "Peer.hpp"
#include "Mutex.hpp"
#include "InetAddress.hpp"
#include "Hashtable.hpp"
#include "World.hpp"

namespace ZeroTier {

class RuntimeEnvironment;

/**
 * Database of network topology
 */
class Topology
{
public:
	Topology(const RuntimeEnvironment *renv);
	~Topology();

	/**
	 * Add a peer to database
	 *
	 * This will not replace existing peers. In that case the existing peer
	 * record is returned.
	 *
	 * @param peer Peer to add
	 * @return New or existing peer (should replace 'peer')
	 */
	SharedPtr<Peer> addPeer(const SharedPtr<Peer> &peer);

	/**
	 * Get a peer from its address
	 *
	 * @param zta ZeroTier address of peer
	 * @return Peer or NULL if not found
	 */
	SharedPtr<Peer> getPeer(const Address &zta);

	/**
	 * @return Vector of peers that are root servers
	 */
	inline std::vector< SharedPtr<Peer> > rootPeers() const
	{
		Mutex::Lock _l(_lock);
		return _rootPeers;
	}

	/**
	 * Get the current favorite root server
	 *
	 * @return Root server with lowest latency or NULL if none
	 */
	inline SharedPtr<Peer> getBestRoot()
	{
		return getBestRoot((const Address *)0,0,false);
	}

	/**
	 * Get the best root server, avoiding root servers listed in an array
	 *
	 * This will get the best root server (lowest latency, etc.) but will
	 * try to avoid the listed root servers, only using them if no others
	 * are available.
	 *
	 * @param avoid Nodes to avoid
	 * @param avoidCount Number of nodes to avoid
	 * @param strictAvoid If false, consider avoided root servers anyway if no non-avoid root servers are available
	 * @return Root server or NULL if none available
	 */
	SharedPtr<Peer> getBestRoot(const Address *avoid,unsigned int avoidCount,bool strictAvoid);

	/**
	 * @param id Identity to check
	 * @return True if this is a designated root server in this world
	 */
	inline bool isRoot(const Identity &id) const
	{
		Mutex::Lock _l(_lock);
		if (std::find(_rootAddresses.begin(),_rootAddresses.end(),id.address()) != _rootAddresses.end()) {
			// Double check full identity for security reasons
			for(std::vector<World::Root>::const_iterator r(_world.roots().begin());r!=_world.roots().end();++r) {
				if (id == r->identity)
					return true;
			}
		}
		return false;
	}

	/**
	 * @return Vector of root server addresses
	 */
	inline std::vector<Address> rootAddresses() const
	{
		Mutex::Lock _l(_lock);
		return _rootAddresses;
	}

	/**
	 * @return Current World (copy)
	 */
	inline World world() const
	{
		Mutex::Lock _l(_lock);
		return _world;
	}

	/**
	 * Clean and flush database
	 */
	void clean(uint64_t now);

	/**
	 * Apply a function or function object to all peers
	 *
	 * Note: explicitly template this by reference if you want the object
	 * passed by reference instead of copied.
	 *
	 * Warning: be careful not to use features in these that call any other
	 * methods of Topology that may lock _lock, otherwise a recursive lock
	 * and deadlock or lock corruption may occur.
	 *
	 * @param f Function to apply
	 * @tparam F Function or function object type
	 */
	template<typename F>
	inline void eachPeer(F f)
	{
		Mutex::Lock _l(_lock);
		Hashtable< Address,SharedPtr<Peer> >::Iterator i(_peers);
		Address *a = (Address *)0;
		SharedPtr<Peer> *p = (SharedPtr<Peer> *)0;
		while (i.next(a,p))
			f(*this,*p);
	}

	/**
	 * @return All currently active peers by address
	 */
	inline std::vector< std::pair< Address,SharedPtr<Peer> > > allPeers() const
	{
		Mutex::Lock _l(_lock);
		return _peers.entries();
	}

private:
	Identity _getIdentity(const Address &zta);
	void _saveIdentity(const Identity &id);

	const RuntimeEnvironment *RR;

	World _world;
	Hashtable< Address,SharedPtr<Peer> > _peers;
	std::vector< Address > _rootAddresses;
	std::vector< SharedPtr<Peer> > _rootPeers;
	bool _amRoot;

	Mutex _lock;
};

} // namespace ZeroTier

#endif
