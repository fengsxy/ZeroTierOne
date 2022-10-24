// (c) 2020-2022 ZeroTier, Inc. -- currently propritery pending actual release and licensing. See LICENSE.md.

use std::error::Error;
use std::sync::{Arc, Mutex, RwLock, Weak};

use tokio::time::{Duration, Instant};

use zerotier_network_hypervisor::protocol;
use zerotier_network_hypervisor::protocol::{PacketBuffer, DEFAULT_MULTICAST_LIMIT, ZEROTIER_VIRTUAL_NETWORK_DEFAULT_MTU};
use zerotier_network_hypervisor::vl1::{HostSystem, Identity, InnerProtocol, Node, PacketHandlerResult, Path, PathFilter, Peer};
use zerotier_network_hypervisor::vl2::{CertificateOfMembership, CertificateOfOwnership, NetworkConfig, NetworkId, Tag};
use zerotier_utils::blob::Blob;
use zerotier_utils::buffer::OutOfBoundsError;
use zerotier_utils::dictionary::Dictionary;
use zerotier_utils::error::InvalidParameterError;
use zerotier_utils::reaper::Reaper;
use zerotier_utils::tokio;
use zerotier_utils::{ms_monotonic, ms_since_epoch};
use zerotier_vl1_service::VL1Service;

use crate::database::*;
use crate::model::{AuthorizationResult, Member, RequestLogItem, CREDENTIAL_WINDOW_SIZE_DEFAULT};

// A netconf per-query task timeout, just a sanity limit.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);

/// ZeroTier VL2 network controller packet handler, answers VL2 netconf queries.
pub struct Handler {
    self_ref: Weak<Self>,
    service: RwLock<Weak<VL1Service<dyn Database, Self, Self>>>,
    reaper: Reaper,
    daemons: Mutex<Vec<tokio::task::JoinHandle<()>>>, // drop() aborts these
    runtime: tokio::runtime::Handle,
    database: Arc<dyn Database>,
    local_identity: Identity,
}

impl Handler {
    /// Start an inner protocol handler answer ZeroTier VL2 network controller queries.
    pub async fn new(database: Arc<dyn Database>, runtime: tokio::runtime::Handle) -> Result<Arc<Self>, Box<dyn Error>> {
        if let Some(local_identity) = database.load_node_identity() {
            assert!(local_identity.secret.is_some());
            Ok(Arc::new_cyclic(|r| Self {
                self_ref: r.clone(),
                service: RwLock::new(Weak::default()),
                reaper: Reaper::new(&runtime),
                daemons: Mutex::new(Vec::with_capacity(1)),
                runtime,
                database: database.clone(),
                local_identity,
            }))
        } else {
            Err(Box::new(InvalidParameterError(
                "local controller's identity not readable by database",
            )))
        }
    }

    /// Set the service and HostSystem implementation for this controller.
    ///
    /// This must be called once the service that uses this handler is up or the controller
    /// won't actually do anything. The reference the handler holds is weak to prevent
    /// a circular reference, so if the VL1Service is dropped this must be called again to
    /// tell the controller handler about a new instance.
    pub fn set_service(&self, service: &Arc<VL1Service<dyn Database, Self, Self>>) {
        *self.service.write().unwrap() = Arc::downgrade(service);
    }

    /// Start a change watcher to respond to changes detected by the database.
    ///
    /// This should only be called once, though multiple calls won't do anything but create
    /// unnecessary async tasks. If the database being used does not support changes, this
    /// does nothing.
    pub async fn start_change_watcher(&self) {
        if let Some(cw) = self.database.changes().await.map(|mut ch| {
            let self2 = self.self_ref.upgrade().unwrap();
            self.runtime.spawn(async move {
                loop {
                    if let Ok(change) = ch.recv().await {
                        self2.reaper.add(
                            self2.runtime.spawn(self2.clone().handle_change_notification(change)),
                            Instant::now().checked_add(REQUEST_TIMEOUT).unwrap(),
                        );
                    }
                }
            })
        }) {
            self.daemons.lock().unwrap().push(cw);
        }
    }
}

// Default PathFilter implementations permit anything.
impl PathFilter for Handler {}

impl InnerProtocol for Handler {
    fn handle_packet<HostSystemImpl: HostSystem + ?Sized>(
        &self,
        _: &HostSystemImpl,
        _: &Node,
        source: &Arc<Peer>,
        source_path: &Arc<Path>,
        source_hops: u8,
        message_id: u64,
        verb: u8,
        payload: &PacketBuffer,
        mut cursor: usize,
    ) -> PacketHandlerResult {
        match verb {
            protocol::verbs::VL2_VERB_NETWORK_CONFIG_REQUEST => {
                let network_id = payload.read_u64(&mut cursor);
                if network_id.is_err() {
                    return PacketHandlerResult::Error;
                }
                let network_id = NetworkId::from_u64(network_id.unwrap());
                if network_id.is_none() {
                    return PacketHandlerResult::Error;
                }
                let network_id = network_id.unwrap();

                let meta_data = if (cursor + 2) < payload.len() {
                    let meta_data_len = payload.read_u16(&mut cursor);
                    if meta_data_len.is_err() {
                        return PacketHandlerResult::Error;
                    }
                    if let Ok(d) = payload.read_bytes(meta_data_len.unwrap() as usize, &mut cursor) {
                        let d = Dictionary::from_bytes(d);
                        if d.is_none() {
                            return PacketHandlerResult::Error;
                        }
                        d.unwrap()
                    } else {
                        return PacketHandlerResult::Error;
                    }
                } else {
                    Dictionary::new()
                };

                /*
                let (have_revision, have_timestamp) = if (cursor + 16) <= payload.len() {
                    let r = payload.read_u64(&mut cursor);
                    let t = payload.read_u64(&mut cursor);
                    if r.is_err() || t.is_err() {
                        return PacketHandlerResult::Error;
                    }
                    (Some(r.unwrap()), Some(t.unwrap()))
                } else {
                    (None, None)
                };
                */

                // Launch handler as an async background task.
                let (self2, peer, source_remote_endpoint) =
                    (self.self_ref.upgrade().unwrap(), source.clone(), source_path.endpoint.clone());
                self.reaper.add(
                    self.runtime.spawn(async move {
                        let node_id = peer.identity.address;
                        let node_fingerprint = Blob::from(peer.identity.fingerprint);
                        let now = ms_since_epoch();

                        let result = match self2.handle_network_config_request(&peer.identity, network_id, now).await {
                            Result::Ok((result, Some(config))) => {
                                self2.send_network_config(peer.as_ref(), &config, Some(message_id));
                                result
                            }
                            Result::Ok((result, None)) => result,
                            Result::Err(_) => {
                                // TODO: log invalid request or internal error
                                return;
                            }
                        };

                        let _ = self2
                            .database
                            .log_request(RequestLogItem {
                                network_id,
                                node_id,
                                node_fingerprint,
                                controller_node_id: self2.local_identity.address,
                                metadata: if meta_data.is_empty() {
                                    Vec::new()
                                } else {
                                    meta_data.to_bytes()
                                },
                                timestamp: now,
                                source_remote_endpoint,
                                source_hops,
                                result,
                            })
                            .await;
                    }),
                    Instant::now().checked_add(REQUEST_TIMEOUT).unwrap(),
                );

                PacketHandlerResult::Ok
            }
            _ => PacketHandlerResult::NotHandled,
        }
    }

    fn handle_error<HostSystemImpl: HostSystem + ?Sized>(
        &self,
        _host_system: &HostSystemImpl,
        _node: &Node,
        _source: &Arc<Peer>,
        _source_path: &Arc<Path>,
        _source_hops: u8,
        _message_id: u64,
        _in_re_verb: u8,
        _in_re_message_id: u64,
        _error_code: u8,
        _payload: &PacketBuffer,
        _cursor: usize,
    ) -> PacketHandlerResult {
        PacketHandlerResult::NotHandled
    }

    fn handle_ok<HostSystemImpl: HostSystem + ?Sized>(
        &self,
        _host_system: &HostSystemImpl,
        _node: &Node,
        _source: &Arc<Peer>,
        _source_path: &Arc<Path>,
        _source_hops: u8,
        _message_id: u64,
        _in_re_verb: u8,
        _in_re_message_id: u64,
        _payload: &PacketBuffer,
        _cursor: usize,
    ) -> PacketHandlerResult {
        PacketHandlerResult::NotHandled
    }

    fn should_respond_to(&self, _: &Identity) -> bool {
        true
    }
}

impl Handler {
    fn send_network_config(
        &self,
        peer: &Peer,
        config: &NetworkConfig,
        in_re_message_id: Option<u64>, // None for unsolicited push
    ) {
        if let Some(host_system) = self.service.read().unwrap().upgrade() {
            peer.send(
                host_system.as_ref(),
                host_system.node(),
                None,
                ms_monotonic(),
                |packet| -> Result<(), OutOfBoundsError> {
                    if let Some(in_re_message_id) = in_re_message_id {
                        let ok_header = packet.append_struct_get_mut::<protocol::OkHeader>()?;
                        ok_header.verb = protocol::verbs::VL1_OK;
                        ok_header.in_re_verb = protocol::verbs::VL2_VERB_NETWORK_CONFIG_REQUEST;
                        ok_header.in_re_message_id = in_re_message_id.to_be_bytes();
                    } else {
                        packet.append_u8(protocol::verbs::VL2_VERB_NETWORK_CONFIG)?;
                    }

                    if peer.is_v2() {
                        todo!()
                    } else {
                        let config_data = if let Some(config_dict) = config.v1_proto_to_dictionary(&self.local_identity) {
                            config_dict.to_bytes()
                        } else {
                            eprintln!("WARNING: unexpected error serializing network config into V1 format dictionary");
                            return Err(OutOfBoundsError); // abort
                        };
                        if config_data.len() > (u16::MAX as usize) {
                            eprintln!("WARNING: network config is larger than 65536 bytes!");
                            return Err(OutOfBoundsError); // abort
                        }

                        packet.append_u64(config.network_id.into())?;
                        packet.append_u16(config_data.len() as u16)?;
                        packet.append_bytes(config_data.as_slice())?;

                        // TODO: compress

                        // NOTE: V1 supports a bunch of other things like chunking but it was never truly used and is optional.
                        // Omit it here as it adds overhead.
                    }

                    Ok(())
                },
            );
            // TODO: log errors
        }
    }

    async fn handle_change_notification(self: Arc<Self>, _change: Change) {}

    async fn handle_network_config_request(
        self: &Arc<Self>,
        source_identity: &Identity,
        network_id: NetworkId,
        now: i64,
    ) -> Result<(AuthorizationResult, Option<NetworkConfig>), Box<dyn Error + Send + Sync>> {
        let network = self.database.get_network(network_id).await?;
        if network.is_none() {
            // TODO: send error
            return Ok((AuthorizationResult::Rejected, None));
        }
        let network = network.unwrap();

        let mut member = self.database.get_member(network_id, source_identity.address).await?;
        let mut member_changed = false;
        let legacy_v1 = source_identity.p384.is_none();

        // If we have a member object and a pinned identity, check to make sure it matches.
        if let Some(member) = member.as_ref() {
            if let Some(pinned_identity) = member.identity.as_ref() {
                if !pinned_identity.eq(&source_identity) {
                    return Ok((AuthorizationResult::RejectedIdentityMismatch, None));
                }
            }
        }

        let mut authorization_result = AuthorizationResult::Rejected;
        let mut authorized = member.as_ref().map_or(false, |m| m.authorized());
        if !authorized {
            if member.is_none() {
                if network.learn_members.unwrap_or(true) {
                    let _ = member.insert(Member::new_with_identity(source_identity.clone(), network_id));
                    member_changed = true;
                } else {
                    return Ok((AuthorizationResult::Rejected, None));
                }
            }

            if !network.private {
                authorization_result = AuthorizationResult::ApprovedOnPublicNetwork;
                authorized = true;
                member.as_mut().unwrap().last_authorized_time = Some(now);
                member_changed = true;
            }
        }

        let mut member = member.unwrap();

        let nc: Option<NetworkConfig> = if authorized {
            // ====================================================================================
            // Authorized requests are handled here
            // ====================================================================================

            // TODO: check SSO

            // Figure out time bounds for the certificate to generate.
            let credential_ttl = network.credential_ttl.unwrap_or(CREDENTIAL_WINDOW_SIZE_DEFAULT);

            // Get a list of all network members that were deauthorized but are still within the time window.
            // These will be issued revocations to remind the node not to speak to them until they fall off.
            let deauthed_members_still_in_window = self
                .database
                .list_members_deauthorized_after(network.id, now - credential_ttl)
                .await;

            // Check and if necessary auto-assign static IPs for this member.
            member_changed |= network.check_zt_ip_assignments(self.database.as_ref(), &mut member).await;

            let mut nc = NetworkConfig::new(network_id, source_identity.address);

            nc.name = member.name.clone();
            nc.private = network.private;
            nc.timestamp = now;
            nc.credential_ttl = credential_ttl;
            nc.revision = now as u64;
            nc.mtu = network.mtu.unwrap_or(ZEROTIER_VIRTUAL_NETWORK_DEFAULT_MTU as u16);
            nc.multicast_limit = network.multicast_limit.unwrap_or(DEFAULT_MULTICAST_LIMIT as u32);
            nc.routes = network.ip_routes;
            nc.static_ips = member.ip_assignments.clone();
            nc.rules = network.rules;
            nc.dns = network.dns;

            nc.certificate_of_membership =
                CertificateOfMembership::new(&self.local_identity, network_id, &source_identity, now, credential_ttl, legacy_v1);
            if nc.certificate_of_membership.is_none() {
                return Ok((AuthorizationResult::RejectedDueToError, None));
            }

            let mut coo = CertificateOfOwnership::new(network_id, now, source_identity.address, legacy_v1);
            for ip in nc.static_ips.iter() {
                coo.add_ip(ip);
            }
            if !coo.sign(&self.local_identity, &source_identity) {
                return Ok((AuthorizationResult::RejectedDueToError, None));
            }
            nc.certificates_of_ownership.push(coo);

            for (id, value) in member.tags.iter() {
                let tag = Tag::new(*id, *value, &self.local_identity, network_id, &source_identity, now, legacy_v1);
                if tag.is_none() {
                    return Ok((AuthorizationResult::RejectedDueToError, None));
                }
                let _ = nc.tags.insert(*id, tag.unwrap());
            }

            // TODO: node info, which isn't supported in v1 so not needed yet

            // TODO: revocations!

            Some(nc)
            // ====================================================================================
        } else {
            None
        };

        if member_changed {
            self.database.save_member(member).await?;
        }

        Ok((authorization_result, nc))
    }
}

impl Drop for Handler {
    fn drop(&mut self) {
        for h in self.daemons.lock().unwrap().drain(..) {
            h.abort();
        }
    }
}
