use std::pin::Pin;
use std::sync::Arc;
use std::sync::mpsc;

use bollard::API_DEFAULT_VERSION;
use bollard::Docker;
use bollard::models::ContainerCreateBody;
use bollard::query_parameters::CreateContainerOptionsBuilder;
use bollard::query_parameters::InspectContainerOptions;
use bollard::query_parameters::KillContainerOptionsBuilder;
use bollard::query_parameters::RemoveContainerOptionsBuilder;
use bollard::query_parameters::StartContainerOptions;
use bollard::query_parameters::StopContainerOptionsBuilder;
use bollard::query_parameters::WaitContainerOptionsBuilder;
use capnp::capability::Promise;
use capnp::message::ReaderOptions;
use capnp_rpc::rpc_twoparty_capnp;
use container_capnp::container;
use futures::AsyncReadExt;
use futures::AsyncWriteExt;
use futures::StreamExt;
use thiserror::Error;
use tokio::runtime::Builder;

pub mod io;
use io::channel;
use io::signo_as_string;
use tokio::sync::Notify;

#[derive(Debug, Error)]
pub enum ContainerError {
    #[error("Socket error: {0}")]
    Socket(#[from] bollard::errors::Error),
    #[error("RPC error: {0}")]
    RpcError(#[from] capnp::Error),
    #[error("Unimplemented: {0}")]
    Unimplemented(String),
}

impl From<ContainerError> for capnp::Error {
    fn from(value: ContainerError) -> Self {
        match value {
            ContainerError::Socket(err) => Self::failed(err.to_string()),
            ContainerError::RpcError(err) => err,
            ContainerError::Unimplemented(msg) => Self::unimplemented(msg),
        }
    }
}

pub struct ContainerService {
    r#impl: Arc<Impl>,
}

#[cxx::bridge(namespace = "workerd::rust::container")]
mod ffi {
    extern "Rust" {
        type ContainerService;
        fn new_service(
            address: &str,
            container_name: &str,
            messages_callback: Pin<&'static mut MessageCallback>,
        ) -> Result<Box<ContainerService>>;
        fn write_data(self: &mut ContainerService, data: &[u8]) -> bool;
        fn shutdown_write(self: &mut ContainerService);
        fn is_write_disconnected(self: &ContainerService) -> bool;
    }

    unsafe extern "C++" {
        include!("workerd/rust/container/bridge.h");
        type MessageCallback;
        #[cxx_name = "operatorCall"]
        fn call(self: Pin<&mut MessageCallback>, message: &[u8]);
    }
}

unsafe impl Send for ffi::MessageCallback {}

/// Creates a new container service.
///
/// # Errors
///
/// Returns an error if the Docker connection fails or if the service cannot be initialized.
pub fn new_service(
    address: &str,
    container_name: &str,
    messages_callback: Pin<&'static mut ffi::MessageCallback>,
) -> Result<Box<ContainerService>, ContainerError> {
    let r#impl = Arc::new(Impl::new(address, container_name, messages_callback)?);
    let service = Box::new(ContainerService { r#impl });
    Ok(service)
}

impl ContainerService {
    pub fn write_data(&mut self, data: &[u8]) -> bool {
        if self
            .r#impl
            .write_shutdown
            .load(std::sync::atomic::Ordering::Relaxed)
        {
            return false;
        }
        self.r#impl.sender.try_send(data.to_vec()).unwrap();
        self.r#impl.notify.notify_one();
        true
    }

    pub fn shutdown_write(&mut self) {
        self.r#impl
            .write_shutdown
            .store(true, std::sync::atomic::Ordering::Relaxed);
    }

    #[must_use]
    pub fn is_write_disconnected(&self) -> bool {
        self.r#impl
            .write_shutdown
            .load(std::sync::atomic::Ordering::Relaxed)
    }
}

struct Impl {
    sender: mpsc::SyncSender<Vec<u8>>,
    notify: Arc<Notify>,
    write_shutdown: std::sync::atomic::AtomicBool,
}

impl Impl {
    pub fn new(
        address: &str,
        container_name: &str,
        mut messages_callback: Pin<&'static mut ffi::MessageCallback>,
    ) -> Result<Self, ContainerError> {
        let (input_sender, input_receiver) = mpsc::sync_channel::<Vec<u8>>(1000);
        let (output_sender, mut output_receiver) = channel();
        let (mut tokio_input_sender, tokio_input_receiver) = channel();
        let output_notify = Arc::new(Notify::new());
        let notifiy_awaiter = output_notify.clone();

        let server = Server::connect(address, container_name)?;
        let runtime = Builder::new_current_thread().enable_all().build().unwrap();

        std::thread::spawn(move || {
            dbg!("SPAWNING TOKIO");
            let local = tokio::task::LocalSet::new();
            local.spawn_local(async move {
                let client: container::Client = capnp_rpc::new_client(server);
                dbg!("INITIALIZING RPC");
                let network = capnp_rpc::twoparty::VatNetwork::new(
                    tokio_input_receiver,
                    output_sender,
                    rpc_twoparty_capnp::Side::Server,
                    ReaderOptions::default(),
                );
                let rpc_system = capnp_rpc::RpcSystem::new(Box::new(network), Some(client.client));

                dbg!("RPC_SYSTEM SPAWNING");
                tokio::task::spawn_local(rpc_system);
            });
            local.spawn_local(async move {
                loop {
                    let mut buf = [0; 8096];
                    let Ok(len) = output_receiver.read(&mut buf).await else {
                        break;
                    };
                    dbg!(len);
                    messages_callback.as_mut().call(&buf[0..len]);
                }
                dbg!("OUTPUT_RECEIVER.RECV() ENDED");
            });
            local.spawn_local(async move {
                loop {
                    notifiy_awaiter.notified().await;
                    while let Ok(msg) = input_receiver.try_recv() {
                        if tokio_input_sender.write(&msg).await.is_err() {
                            break;
                        }
                    }
                }
                dbg!("INPUT_RECEIVER.RECV() ENDED");
            });
            runtime.block_on(local);
            dbg!("RPC_SYSTEM DONE");
        });

        Ok(Impl {
            sender: input_sender,
            notify: output_notify,
            write_shutdown: std::sync::atomic::AtomicBool::new(false),
        })
    }
}

struct Server {
    docker: Arc<Docker>,
    container_name: String,
}

impl Server {
    fn connect(address: &str, container_name: &str) -> Result<Self, ContainerError> {
        dbg!("CONNECT");
        let docker = Docker::connect_with_socket(address, 120, API_DEFAULT_VERSION)?;
        Ok(Server {
            docker: docker.into(),
            container_name: container_name.to_owned(),
        })
    }
}

impl container::Server for Server {
    fn destroy(
        &mut self,
        _: container::DestroyParams,
        _: container::DestroyResults,
    ) -> Promise<(), capnp::Error> {
        dbg!("DESTROY");
        let container_name = self.container_name.clone();
        let docker = self.docker.clone();
        Promise::from_future(async move {
            let options = StopContainerOptionsBuilder::default().signal("SIGINT");
            docker
                .stop_container(&container_name, Some(options.build()))
                .await
                .map_err(ContainerError::Socket)?;

            let remove_options = RemoveContainerOptionsBuilder::default().force(true);
            docker
                .remove_container(&container_name, Some(remove_options.build()))
                .await
                .map_err(ContainerError::Socket)?;
            Ok(())
        })
    }

    fn signal(
        &mut self,
        params: container::SignalParams,
        _: container::SignalResults,
    ) -> Promise<(), capnp::Error> {
        dbg!("SIGNAL");
        let container_name = self.container_name.clone();
        let docker = self.docker.clone();
        Promise::from_future(async move {
            let signal =
                signo_as_string(params.get()?.get_signo()).unwrap_or_else(|| "SIGINT".to_owned());

            let options = KillContainerOptionsBuilder::default().signal(&signal);
            docker
                .kill_container(&container_name, Some(options.build()))
                .await
                .map_err(ContainerError::Socket)?;
            Ok(())
        })
    }

    /// Starts a container.
    /// This method doesn't handle enableInternet attribute since
    /// it doesn't make sense for the local usage, and will be hard to
    /// manage while still having port mapping to the host.
    fn start(
        &mut self,
        raw_params: container::StartParams,
        _: container::StartResults,
    ) -> Promise<(), capnp::Error> {
        dbg!("START");
        let container_name = self.container_name.clone();
        let docker = self.docker.clone();
        Promise::from_future(async move {
            let params = raw_params.get()?;
            let entrypoint = params
                .get_entrypoint()?
                .iter()
                .map(|s| s.unwrap().to_string().unwrap())
                .collect::<Vec<String>>();
            let env = params
                .get_environment_variables()?
                .iter()
                .map(|s| s.unwrap().to_string().unwrap())
                .collect::<Vec<String>>();

            let config = ContainerCreateBody {
                image: Some(container_name.clone()),
                cmd: Some(entrypoint),
                env: Some(env),
                ..Default::default()
            };

            let options = CreateContainerOptionsBuilder::default()
                .name(&container_name)
                .build();

            docker
                .create_container(Some(options), config)
                .await
                .map_err(|e| capnp::Error::failed(e.to_string()))?;

            docker
                .start_container(&container_name, None::<StartContainerOptions>)
                .await
                .map_err(|e| capnp::Error::failed(e.to_string()))?;

            Ok(())
        })
    }

    fn status(
        &mut self,
        _: container::StatusParams,
        mut results: container::StatusResults,
    ) -> Promise<(), capnp::Error> {
        dbg!("STATUS");
        let container_name = self.container_name.clone();
        let docker = self.docker.clone();
        Promise::from_future(async move {
            dbg!("querying status");
            let inspect = docker
                .inspect_container(&container_name, None::<InspectContainerOptions>)
                .await
                .map_err(|e| capnp::Error::failed(e.to_string()))?;
            dbg!(&inspect);
            let mut builder = results.get();
            if let Some(state) = inspect.state {
                builder.set_running(state.running.unwrap_or(false));
            }
            dbg!(builder.reborrow_as_reader());
            Ok(())
        })
    }

    fn monitor(
        &mut self,
        _: container::MonitorParams,
        _: container::MonitorResults,
    ) -> Promise<(), capnp::Error> {
        dbg!("MONITOR");
        let container_name = self.container_name.clone();
        let docker = self.docker.clone();
        Promise::from_future(async move {
            let options = WaitContainerOptionsBuilder::default()
                .condition("not-running")
                .build();

            let mut stream = docker.wait_container(&container_name, Some(options));

            if let Some(wait_result) = stream.next().await {
                let status_code = wait_result.map_err(ContainerError::Socket)?.status_code;
                if status_code == 0 {
                    return Ok(());
                }

                return Err(capnp::Error::failed(format!(
                    "Container exited with unexpected exit code: {status_code}"
                )));
            }

            // Stream ended without result - should not happen
            Err(capnp::Error::failed(
                "Monitoring stream ended unexpectedly".to_owned(),
            ))
        })
    }

    /// This method is intentionally not implemented right now.
    fn listen_tcp(
        &mut self,
        _params: container::ListenTcpParams,
        _results: container::ListenTcpResults,
    ) -> Promise<(), capnp::Error> {
        dbg!("LISTEN_TCP");
        Promise::from_future(async move {
            Err(capnp::Error::unimplemented(
                "listen_tcp is not implemented".to_owned(),
            ))
        })
    }

    /// TODO(soon): Implement this.
    fn get_tcp_port(
        &mut self,
        _params: container::GetTcpPortParams,
        _results: container::GetTcpPortResults,
    ) -> Promise<(), capnp::Error> {
        dbg!("GET_TCP_PORT");
        Promise::from_future(async move {
            Err(capnp::Error::unimplemented(
                "get_tcp_port is not implemented".to_owned(),
            ))
        })
    }
}
