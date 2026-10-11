//! Private C ABI for the C++ packet adapter. Gameplay and room state stay in C++.

use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use bytes::Bytes;
use iroh::{
    Endpoint, EndpointAddr, RelayMode,
    endpoint::{Connection, QuicTransportConfig, RecvStream, SendStream, presets},
};
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, VecDeque},
    panic::{AssertUnwindSafe, catch_unwind},
    sync::{
        Arc, Mutex,
        atomic::{AtomicU32, Ordering},
    },
    thread,
    time::{Duration, Instant},
};
use subtle::ConstantTimeEq;
use tokio::sync::{OwnedSemaphorePermit, Semaphore, mpsc, oneshot};

const ALPN: &[u8] = b"gauntlet-dark-legacy/netplay/1";
const MAX_PACKET: usize = 1201; // One room-envelope byte plus PacketTransport's 1200.
const FRAGMENT: usize = 1000; // Fits QUIC even on its minimum supported path MTU.
const SEND_BYTES: usize = 65536;
const INVITE_LIMIT: usize = 2048;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(20);
const AUTH_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Serialize, Deserialize)]
struct Ticket {
    version: u8,
    address: EndpointAddr,
    token: [u8; 32],
}
impl Ticket {
    fn encode(&self) -> Option<String> {
        let text = format!(
            "gdl1-{}",
            URL_SAFE_NO_PAD.encode(postcard::to_allocvec(self).ok()?)
        );
        (text.len() <= INVITE_LIMIT).then_some(text)
    }
    fn decode(text: &str) -> Option<Self> {
        let text = text.trim();
        if text.len() > INVITE_LIMIT {
            return None;
        }
        let bytes = URL_SAFE_NO_PAD.decode(text.strip_prefix("gdl1-")?).ok()?;
        let (ticket, rest): (Self, _) = postcard::take_from_bytes(&bytes).ok()?;
        if !rest.is_empty()
            || ticket.version != 1
            || ticket.token == [0; 32]
            || ticket.address.addrs.is_empty()
            || ticket.address.addrs.len() > 16
        {
            return None;
        }
        Some(ticket)
    }
}

#[derive(Default)]
struct Shared {
    state: u32,
    invite: String,
    error: String,
    links: BTreeMap<u64, Arc<Link>>,
}
struct Link {
    connection: Connection,
    reliable: mpsc::Sender<Outgoing>,
    budget: Arc<Semaphore>,
    sequence: AtomicU32,
}
struct Outgoing {
    bytes: Vec<u8>,
    _permit: OwnedSemaphorePermit,
}
struct Event {
    kind: u32,
    id: u64,
    bytes: Vec<u8>,
}
impl Event {
    fn connected(id: u64) -> Self {
        Self {
            kind: 0,
            id,
            bytes: vec![],
        }
    }
    fn disconnected(id: u64) -> Self {
        Self {
            kind: 1,
            id,
            bytes: vec![],
        }
    }
    fn message(id: u64, bytes: Vec<u8>) -> Self {
        Self { kind: 2, id, bytes }
    }
}

/// One owned runtime thread. C++ never blocks on DNS, handshakes or a stream write.
pub struct Handle {
    shared: Arc<Mutex<Shared>>,
    events: mpsc::Receiver<Event>,
    stop: Option<oneshot::Sender<()>>,
    worker: Option<thread::JoinHandle<()>>,
}
impl Drop for Handle {
    fn drop(&mut self) {
        if let Some(stop) = self.stop.take() {
            let _ = stop.send(());
        }
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}
impl Handle {
    fn new(invite: &str, local: bool) -> Option<Self> {
        let ticket = if invite.is_empty() {
            None
        } else {
            Some(Ticket::decode(invite)?)
        };
        if local
            && ticket
                .as_ref()
                .is_some_and(|ticket| ticket.address.relay_urls().next().is_some())
        {
            return None;
        }
        let shared = Arc::new(Mutex::new(Shared::default()));
        let (events_tx, events) = mpsc::channel(512);
        let (stop, stopped) = oneshot::channel();
        let state = shared.clone();
        let worker = thread::Builder::new()
            .name("netplay-io".into())
            .spawn(move || {
                let result = catch_unwind(AssertUnwindSafe(|| {
                    let runtime = tokio::runtime::Builder::new_current_thread()
                        .enable_all()
                        .build()
                        .map_err(|_| "Unable to start network worker")?;
                    runtime.block_on(run(state.clone(), events_tx, stopped, ticket, local))
                }));
                let error = match result {
                    Ok(Ok(())) => None,
                    Ok(Err(message)) => Some(message),
                    Err(_) => Some("Network worker stopped unexpectedly"),
                };
                if let Some(error) = error {
                    let mut state = state.lock().unwrap_or_else(|e| e.into_inner());
                    state.state = 2;
                    state.error = error.into();
                    state.links.clear();
                }
            })
            .ok()?;
        Some(Self {
            shared,
            events,
            stop: Some(stop),
            worker: Some(worker),
        })
    }
    fn send(&self, id: u64, bytes: &[u8], reliable: bool) -> u32 {
        if bytes.is_empty() || bytes.len() > MAX_PACKET {
            return 3;
        }
        let link = self.shared.lock().unwrap().links.get(&id).cloned();
        let Some(link) = link else {
            return 2;
        };
        if link.connection.close_reason().is_some() {
            return 2;
        }
        if reliable {
            let Ok(permit) = link
                .budget
                .clone()
                .try_acquire_many_owned((bytes.len() + 2) as u32)
            else {
                return 1;
            };
            match link.reliable.try_send(Outgoing {
                bytes: bytes.to_vec(),
                _permit: permit,
            }) {
                Ok(()) => 0,
                Err(mpsc::error::TrySendError::Full(_)) => 1,
                Err(mpsc::error::TrySendError::Closed(_)) => 2,
            }
        } else {
            if link.connection.max_datagram_size().unwrap_or(0) < FRAGMENT + 6 {
                return 1;
            }
            let sequence = link.sequence.fetch_add(1, Ordering::Relaxed);
            let count = bytes.len().div_ceil(FRAGMENT) as u8;
            for (index, part) in bytes.chunks(FRAGMENT).enumerate() {
                let mut packet = Vec::with_capacity(part.len() + 6);
                packet.extend(sequence.to_le_bytes());
                packet.extend([index as u8, count]);
                packet.extend(part);
                if link.connection.send_datagram(Bytes::from(packet)).is_err() {
                    return 2;
                }
            }
            0
        }
    }
}

async fn run(
    shared: Arc<Mutex<Shared>>,
    events: mpsc::Sender<Event>,
    mut stop: oneshot::Receiver<()>,
    ticket: Option<Ticket>,
    local: bool,
) -> Result<(), &'static str> {
    let config = QuicTransportConfig::builder()
        .max_concurrent_bidi_streams(1u32.into())
        .max_concurrent_uni_streams(0u32.into())
        .send_window(SEND_BYTES as u64)
        .receive_window((SEND_BYTES as u32).into())
        .stream_receive_window((SEND_BYTES as u32).into())
        .datagram_receive_buffer_size(Some(SEND_BYTES))
        .datagram_send_buffer_size(SEND_BYTES)
        .build();
    // Tickets carry the addressing information. No public room directory, DNS
    // publication, telemetry account or automatic router mapping is required.
    let binding = Endpoint::builder(presets::Minimal)
        .relay_mode(if local {
            RelayMode::Disabled
        } else {
            RelayMode::Default
        })
        .alpns(vec![ALPN.to_vec()])
        .transport_config(config)
        .bind();
    let endpoint = tokio::select! {
        _ = &mut stop => return Ok(()),
        result = binding => result.map_err(|_| "Unable to open network endpoint")?,
    };
    let work = async {
        if let Some(ticket) = ticket {
            let connection =
                tokio::time::timeout(CONNECT_TIMEOUT, endpoint.connect(ticket.address, ALPN))
                    .await
                    .map_err(|_| "Connection timed out")?
                    .map_err(|_| "Unable to reach the host")?;
            let streams =
                tokio::time::timeout(AUTH_TIMEOUT, authenticate(&connection, ticket.token, false))
                    .await
                    .map_err(|_| "Invitation check timed out")??;
            shared.lock().unwrap().state = 1;
            serve(shared.clone(), events.clone(), connection, streams, 1).await;
        } else {
            if !local {
                tokio::time::timeout(CONNECT_TIMEOUT, endpoint.online())
                    .await
                    .map_err(|_| "No relay is reachable; check your Internet connection")?;
            }
            let mut token = [0; 32];
            getrandom::fill(&mut token).map_err(|_| "Unable to create a private invitation")?;
            let invite = Ticket {
                version: 1,
                address: endpoint.addr(),
                token,
            }
            .encode()
            .ok_or("Unable to encode invitation")?;
            {
                let mut state = shared.lock().unwrap();
                state.invite = invite;
                state.state = 1;
            }
            let limit = Arc::new(Semaphore::new(4));
            let mut tasks = tokio::task::JoinSet::new();
            let mut next_id = 1;
            loop {
                tokio::select! {
                    incoming = endpoint.accept() => {
                        let Some(incoming) = incoming else { break; };
                        let Ok(permit) = limit.clone().try_acquire_owned() else { incoming.refuse(); continue; };
                        let id = next_id; next_id += 1;
                        let state = shared.clone(); let sink = events.clone();
                        tasks.spawn(async move {
                            let _permit = permit;
                            let Ok(Ok(connection)) = tokio::time::timeout(AUTH_TIMEOUT, incoming).await else { return; };
                            let Ok(Ok(streams)) = tokio::time::timeout(AUTH_TIMEOUT, authenticate(&connection, token, true)).await
                                else { connection.close(1u32.into(), b"Invitation rejected"); return; };
                            serve(state, sink, connection, streams, id).await;
                        });
                    },
                    _ = tasks.join_next(), if !tasks.is_empty() => {},
                }
            }
        }
        Ok(())
    };
    let result = tokio::select! { _ = &mut stop => Ok(()), result = work => result };
    let _ = tokio::time::timeout(Duration::from_secs(1), endpoint.close()).await;
    result
}

async fn authenticate(
    connection: &Connection,
    token: [u8; 32],
    host: bool,
) -> Result<(SendStream, RecvStream), &'static str> {
    let (mut send, mut recv) = if host {
        connection.accept_bi().await
    } else {
        connection.open_bi().await
    }
    .map_err(|_| "Connection closed during invitation check")?;
    if host {
        let mut proof = [0; 32];
        recv.read_exact(&mut proof)
            .await
            .map_err(|_| "Missing invitation")?;
        if !bool::from(proof.ct_eq(&token)) {
            return Err("Invitation rejected");
        }
        send.write_all(&[1])
            .await
            .map_err(|_| "Connection closed during invitation check")?;
    } else {
        send.write_all(&token)
            .await
            .map_err(|_| "Connection closed during invitation check")?;
        let mut accepted = [0];
        recv.read_exact(&mut accepted)
            .await
            .map_err(|_| "Invitation rejected")?;
        if accepted != [1] {
            return Err("Invitation rejected");
        }
    }
    Ok((send, recv))
}

async fn serve(
    shared: Arc<Mutex<Shared>>,
    events: mpsc::Sender<Event>,
    connection: Connection,
    (mut send, mut recv): (SendStream, RecvStream),
    id: u64,
) {
    let (tx, mut rx) = mpsc::channel::<Outgoing>(64);
    let link = Arc::new(Link {
        connection: connection.clone(),
        reliable: tx,
        budget: Arc::new(Semaphore::new(SEND_BYTES)),
        sequence: AtomicU32::new(0),
    });
    shared.lock().unwrap().links.insert(id, link);
    if events.send(Event::connected(id)).await.is_ok() {
        let reader = async {
            loop {
                let mut length = [0; 2];
                recv.read_exact(&mut length).await.ok()?;
                let length = u16::from_le_bytes(length) as usize;
                if length == 0 || length > MAX_PACKET {
                    return None::<()>;
                }
                let mut packet = vec![0; length];
                recv.read_exact(&mut packet).await.ok()?;
                events.send(Event::message(id, packet)).await.ok()?;
            }
        };
        let writer = async {
            while let Some(packet) = rx.recv().await {
                send.write_all(&(packet.bytes.len() as u16).to_le_bytes())
                    .await
                    .ok()?;
                send.write_all(&packet.bytes).await.ok()?;
            }
            Some(())
        };
        let datagrams = async {
            let mut fragments = Fragments::default();
            loop {
                let Ok(bytes) = connection.read_datagram().await else {
                    break;
                };
                if let Some(packet) = fragments.receive(&bytes) {
                    // Stale snapshots may be dropped; control events may not.
                    let _ = events.try_send(Event::message(id, packet));
                }
            }
        };
        tokio::select! { _ = reader => {}, _ = writer => {}, _ = datagrams => {}, _ = connection.closed() => {} }
    }
    connection.close(0u32.into(), b"Session ended");
    shared.lock().unwrap().links.remove(&id);
    let _ = events.send(Event::disconnected(id)).await;
}

#[derive(Default)]
struct Fragments {
    pending: VecDeque<PartialPacket>,
}
struct PartialPacket {
    sequence: u32,
    arrived: Instant,
    parts: [Option<Vec<u8>>; 2],
}
impl Fragments {
    fn receive(&mut self, bytes: &[u8]) -> Option<Vec<u8>> {
        if bytes.len() < 7 || bytes.len() > FRAGMENT + 6 {
            return None;
        }
        let sequence = u32::from_le_bytes(bytes[..4].try_into().ok()?);
        let index = bytes[4] as usize;
        let count = bytes[5];
        if count == 1 && index == 0 {
            return Some(bytes[6..].to_vec());
        }
        if count != 2
            || index > 1
            || (index == 0 && bytes.len() != FRAGMENT + 6)
            || (index == 1 && bytes.len() > MAX_PACKET - FRAGMENT + 6)
        {
            return None;
        }
        self.pending
            .retain(|packet| packet.arrived.elapsed() < Duration::from_millis(250));
        let position = if let Some(position) = self
            .pending
            .iter()
            .position(|packet| packet.sequence == sequence)
        {
            position
        } else {
            if self.pending.len() == 32 {
                self.pending.pop_front();
            }
            self.pending.push_back(PartialPacket {
                sequence,
                arrived: Instant::now(),
                parts: [None, None],
            });
            self.pending.len() - 1
        };
        let parts = &mut self.pending[position].parts;
        parts[index] = Some(bytes[6..].to_vec());
        if parts.iter().any(Option::is_none) {
            return None;
        }
        let parts = self.pending.remove(position)?.parts;
        Some(parts.into_iter().flatten().flatten().collect())
    }
}

#[repr(C)]
pub struct WireEvent {
    pub connection: u64,
    pub kind: u32,
    pub size: u32,
    pub bytes: [u8; MAX_PACKET],
}
#[repr(C)]
pub struct WireStats {
    pub ping_ms: i32,
    pub pending_bytes: i32,
    pub relayed: u32,
}

// Only the C++ adapter owns these opaque pointers. It calls them on one thread,
// supplies the stated array extents, and never accesses a destroyed handle.
// Unwinding is caught at every ABI boundary, never crossing into C++.
/// # Safety
/// A nonempty invitation must point to `length` readable bytes for this call.
/// The returned handle must be destroyed once and accessed on one caller thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gdl_iroh_create(
    invite: *const u8,
    length: usize,
    local: u32,
) -> *mut Handle {
    catch_unwind(|| {
        if length > INVITE_LIMIT || (length > 0 && invite.is_null()) {
            return std::ptr::null_mut();
        }
        let bytes = if length == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(invite, length) }
        };
        let Ok(text) = std::str::from_utf8(bytes) else {
            return std::ptr::null_mut();
        };
        Handle::new(text, local != 0).map_or(std::ptr::null_mut(), |handle| {
            Box::into_raw(Box::new(handle))
        })
    })
    .unwrap_or(std::ptr::null_mut())
}
/// # Safety
/// `handle` must be null or a live handle returned by create, with no concurrent
/// callers. Ownership is consumed; the pointer must not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gdl_iroh_destroy(handle: *mut Handle) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !handle.is_null() {
            drop(unsafe { Box::from_raw(handle) });
        }
    }));
}
/// # Safety
/// `handle` must be null or live and must not be accessed concurrently by C++.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gdl_iroh_state(handle: *const Handle) -> u32 {
    catch_unwind(AssertUnwindSafe(|| {
        unsafe { handle.as_ref() }.map_or(2, |h| h.shared.lock().unwrap().state)
    }))
    .unwrap_or(2)
}
/// # Safety
/// `handle` must be null or live on the caller thread. Nonnull `output` must
/// reference `capacity` writable bytes, without aliasing the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gdl_iroh_text(
    handle: *const Handle,
    ticket: u32,
    output: *mut u8,
    capacity: usize,
) -> usize {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return 0;
        };
        let state = handle.shared.lock().unwrap();
        let text = if ticket != 0 {
            &state.invite
        } else {
            &state.error
        };
        if output.is_null() || text.len() > capacity {
            return 0;
        }
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), output, text.len());
        }
        text.len()
    }))
    .unwrap_or(0)
}
/// # Safety
/// Nonnull pointers must reference live, properly aligned objects, exclusively
/// borrowed for this call; `event` must not alias the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gdl_iroh_poll(handle: *mut Handle, event: *mut WireEvent) -> u32 {
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(handle), Some(event)) = (unsafe { handle.as_mut() }, unsafe { event.as_mut() })
        else {
            return 0;
        };
        let Ok(next) = handle.events.try_recv() else {
            return 0;
        };
        event.connection = next.id;
        event.kind = next.kind;
        event.size = next.bytes.len() as u32;
        event.bytes[..next.bytes.len()].copy_from_slice(&next.bytes);
        1
    }))
    .unwrap_or(0)
}
/// # Safety
/// `handle` must be null or live on the caller thread. Nonnull `bytes` must
/// reference `length` readable bytes for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gdl_iroh_send(
    handle: *const Handle,
    id: u64,
    bytes: *const u8,
    length: usize,
    reliable: u32,
) -> u32 {
    catch_unwind(AssertUnwindSafe(|| {
        if bytes.is_null() || length == 0 || length > MAX_PACKET {
            return 3;
        }
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return 2;
        };
        handle.send(
            id,
            unsafe { std::slice::from_raw_parts(bytes, length) },
            reliable != 0,
        )
    }))
    .unwrap_or(2)
}
/// # Safety
/// `handle` must be null or live and must not be accessed concurrently by C++.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gdl_iroh_close(handle: *const Handle, id: u64) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if let Some(handle) = unsafe { handle.as_ref() }
            && let Some(link) = handle.shared.lock().unwrap().links.remove(&id)
        {
            link.connection.close(0u32.into(), b"Session ended");
        }
    }));
}
/// # Safety
/// `handle` must be null or live on the caller thread. Nonnull `output` must
/// reference a writable, aligned WireStats which does not alias the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gdl_iroh_stats(
    handle: *const Handle,
    id: u64,
    output: *mut WireStats,
) -> u32 {
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(handle), Some(output)) = (unsafe { handle.as_ref() }, unsafe { output.as_mut() })
        else {
            return 0;
        };
        let state = handle.shared.lock().unwrap();
        let Some(link) = state.links.get(&id) else {
            return 0;
        };
        output.ping_ms = -1;
        output.relayed = 0;
        output.pending_bytes = (SEND_BYTES - link.budget.available_permits()) as i32;
        if let Some(path) = link
            .connection
            .paths()
            .iter()
            .find(|path| path.is_selected())
        {
            output.ping_ms = path.rtt().as_millis().min(i32::MAX as u128) as i32;
            output.relayed = path.is_relay() as u32;
        }
        1
    }))
    .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn until(mut test: impl FnMut() -> bool) {
        let start = Instant::now();
        while !test() {
            assert!(
                start.elapsed() < Duration::from_secs(10),
                "network deadline exceeded"
            );
            thread::sleep(Duration::from_millis(2));
        }
    }
    fn host() -> Handle {
        let host = Handle::new("", true).unwrap();
        until(|| host.shared.lock().unwrap().state != 0);
        assert_eq!(host.shared.lock().unwrap().state, 1);
        host
    }
    fn pair() -> (Handle, Handle, u64) {
        let mut host = host();
        let ticket = host.shared.lock().unwrap().invite.clone();
        let mut guest = Handle::new(&ticket, true).unwrap();
        let mut host_id = 0;
        let mut guest_ready = false;
        until(|| {
            while let Ok(event) = host.events.try_recv() {
                if event.kind == 0 {
                    host_id = event.id;
                }
            }
            while let Ok(event) = guest.events.try_recv() {
                if event.kind == 0 {
                    guest_ready = true;
                }
            }
            host_id != 0 && guest_ready
        });
        (host, guest, host_id)
    }
    #[test]
    fn maximum_packets_and_reliable_order_cross_real_quic() {
        let (mut host, mut guest, id) = pair();
        for sequence in 0..40u8 {
            let mut bytes = vec![sequence; MAX_PACKET];
            bytes[0] = sequence;
            assert_eq!(guest.send(1, &bytes, true), 0);
        }
        let mut received = 0u8;
        until(|| {
            while let Ok(event) = host.events.try_recv() {
                assert_eq!(event.kind, 2);
                assert_eq!(event.bytes, vec![received; MAX_PACKET]);
                received += 1;
            }
            received == 40
        });
        let bytes = vec![99; MAX_PACKET];
        assert_eq!(host.send(id, &bytes, false), 0);
        until(|| {
            guest
                .events
                .try_recv()
                .is_ok_and(|event| event.kind == 2 && event.bytes == bytes)
        });
        assert_eq!(host.send(id, &[], true), 3);
        assert_eq!(host.send(id, &vec![0; MAX_PACKET + 1], false), 3);
        assert_eq!(host.send(999, &[1], true), 2);
        drop(guest);
        until(|| {
            host.events
                .try_recv()
                .is_ok_and(|event| event.kind == 1 && event.id == id)
        });
    }
    #[test]
    fn wrong_invitation_never_admits_a_peer_and_does_not_break_host() {
        let mut host = host();
        let mut ticket = Ticket::decode(&host.shared.lock().unwrap().invite).unwrap();
        ticket.token[0] ^= 1;
        let guest = Handle::new(&ticket.encode().unwrap(), true).unwrap();
        until(|| guest.shared.lock().unwrap().state == 2);
        assert!(host.events.try_recv().is_err());
        assert_eq!(host.shared.lock().unwrap().state, 1);
        assert!(host.shared.lock().unwrap().links.is_empty());
    }
    #[test]
    fn tickets_are_bounded_versioned_and_strict() {
        let host = host();
        let text = host.shared.lock().unwrap().invite.clone();
        let mut ticket = Ticket::decode(&text).unwrap();
        assert!(ticket.address.relay_urls().next().is_none());
        assert!(Ticket::decode(&format!(" \r\n{text} \n")).is_some());
        for bad in [
            String::new(),
            "gdl1-!".into(),
            "x".repeat(INVITE_LIMIT + 1),
            text + "AAAA",
        ] {
            assert!(Ticket::decode(&bad).is_none());
        }
        ticket.version = 2;
        assert!(Ticket::decode(&ticket.encode().unwrap()).is_none());
        ticket.version = 1;
        ticket.token = [0; 32];
        assert!(Ticket::decode(&ticket.encode().unwrap()).is_none());
        ticket.token = [1; 32];
        ticket.address.addrs.insert(iroh::TransportAddr::Relay(
            "https://relay.invalid".parse().unwrap(),
        ));
        assert!(Handle::new(&ticket.encode().unwrap(), true).is_none());
    }
    #[test]
    fn fragments_accept_reordering_but_reject_oversize_and_bound_memory() {
        let first = [&[1, 0, 0, 0, 0, 2][..], &vec![7; FRAGMENT]].concat();
        let last = [&[1, 0, 0, 0, 1, 2][..], &vec![7; MAX_PACKET - FRAGMENT]].concat();
        let mut receiver = Fragments::default();
        assert!(receiver.receive(&last).is_none());
        assert_eq!(receiver.receive(&first), Some(vec![7; MAX_PACKET]));
        assert!(receiver.pending.is_empty());
        assert!(receiver.receive(&[1, 0, 0, 0, 0, 3, 9]).is_none());
        let mut oversize = last;
        oversize.push(0);
        assert!(receiver.receive(&oversize).is_none());
        for sequence in 1..200u32 {
            let mut packet = first.clone();
            packet[..4].copy_from_slice(&sequence.to_le_bytes());
            assert!(receiver.receive(&packet).is_none());
        }
        assert_eq!(receiver.pending.len(), 32);
    }
    #[test]
    fn cancellation_does_not_wait_for_connect_deadlines() {
        let host = host();
        let mut ticket = Ticket::decode(&host.shared.lock().unwrap().invite).unwrap();
        ticket.address.id = iroh::SecretKey::generate().public();
        let start = Instant::now();
        drop(Handle::new(&ticket.encode().unwrap(), true).unwrap());
        assert!(start.elapsed() < Duration::from_secs(2));
    }
}
