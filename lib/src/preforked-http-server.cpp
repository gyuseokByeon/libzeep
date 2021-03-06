// Copyright Maarten L. Hekkelman, Radboud University 2008-2013.
//        Copyright Maarten L. Hekkelman, 2014-2019
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <zeep/config.hpp>

#if SOAP_SERVER_HAS_PREFORK

#include <iostream>
#include <sstream>
#include <locale>
#include <cstdlib>

#include <zeep/http/preforked-server.hpp>
#include <zeep/http/connection.hpp>
#include <zeep/exception.hpp>

#include <boost/date_time/posix_time/posix_time.hpp>

#include <sys/wait.h>

namespace zeep { namespace http {

preforked_server::preforked_server(std::function<server*(void)> constructor)
	: m_constructor(constructor)
	, m_acceptor(m_io_service)
	, m_socket(m_io_service)
{
	m_lock.lock();
}

preforked_server::~preforked_server()
{
	if (m_pid > 0)		// should never happen
	{
		kill(m_pid, SIGKILL);

		int status;
		waitpid(m_pid, &status, WUNTRACED | WCONTINUED);
	}

	m_io_service.stop();
}

void preforked_server::run(const std::string& address, short port, int nr_of_threads)
{
	try
	{
		// create a socket pair to pass the file descriptors through
		int sockfd[2];
		int err = socketpair(AF_LOCAL, SOCK_STREAM, 0, sockfd);
		if (err < 0)
			throw exception("Error creating socket pair: %s", strerror(errno));
	
		// fork
		m_pid = fork();
		if (m_pid < 0)
			throw exception("Error forking worker application: %s", strerror(errno));

		if (m_pid == 0)	// child process
		{
			close(sockfd[0]);
			
			// remove the blocks on the signal handlers
			sigset_t wait_mask;
			sigemptyset(&wait_mask);
			pthread_sigmask(SIG_SETMASK, &wait_mask, 0);
	
			// Time to construct the Server object
			std::unique_ptr<server> srvr(m_constructor());
			
			// run the server as a worker
			std::thread t(std::bind(&server::run, srvr.get(), nr_of_threads));

			// now start the processing loop passing on file descriptors read
			// from the parent process
			try
			{
				for (;;)
				{
					std::shared_ptr<connection> conn(new connection(srvr->get_io_service(), *srvr));
					
					if (not read_socket_from_parent(sockfd[1], conn->get_socket()))
						break;
					
					conn->start();
				}
			}
			catch (std::exception& e)
			{
				std::cerr << "Exception caught: " << e.what() << std::endl;
				exit(1);
			}
			
			srvr->stop();
			t.join();
			
			exit(0);
		}

		// first wait until we are allowed to start listening
		std::unique_lock<std::mutex> lock(m_lock);

		// then bind the address here
		boost::asio::ip::tcp::resolver resolver(m_io_service);
		boost::asio::ip::tcp::resolver::query query(address, std::to_string(port));
		boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(query);
	
		m_acceptor.open(endpoint.protocol());
		m_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
		m_acceptor.bind(endpoint);
		m_acceptor.listen();
	
		m_acceptor.async_accept(m_socket,
			[this](boost::system::error_code ec) { handle_accept(ec); });
		
		// close one end of the pipe, save the other
		m_fd = sockfd[0];
		close(sockfd[1]);
	
		// keep the server at work until we call stop
		boost::asio::io_service::work work(m_io_service);
	
		// start a thread to listen to the socket
		std::thread thread([this]() { m_io_service.run(); });
		
		// now run until the child proces dies, or the server is stopped
		while (not m_io_service.stopped())
		{
			int status;
			
			int r = waitpid(m_pid, &status, WUNTRACED | WCONTINUED);
			if (r == -1)
				throw exception("Error waiting for child process: %s", strerror(errno));
			
			if (WIFEXITED(status))
			{
				m_pid = -1;
				
				std::cerr << "Child exited, status=" << WEXITSTATUS(status) << std::endl;
				break;
			}
			
			if (WIFSIGNALED(status))
			{
				m_pid = -1;

				std::cerr << "Child killed by signal " << WTERMSIG(status) << std::endl;
				break;
			}
			
			// some other reason, simply continue
		}

		thread.join();

		if (m_fd > 0)
		{
			// close the socket to the worker, this should terminate the child if it is still alive
			close(m_fd);
			m_fd = -1;
		}
		
		if (m_pid != -1)
		{
			// however, sometimes it doesn't, so we have to take some serious action
			// Anyway, we'll wait until child dies to avoid zombies
			// and to make sure the client really stops...
			
			int count = 5;	// wait five seconds before killing client
			int status;
	
			while (count-- > 0)
			{
				if (waitpid(m_pid, &status, WUNTRACED | WCONTINUED | WNOHANG) == -1)
					break;
				
				if (WIFEXITED(status))
					break;
				
				sleep(1);
			}
	
			if (not WIFEXITED(status))
				kill(m_pid, SIGKILL);
		}
	}
	catch (std::exception& e)
	{
		std::cerr << "Exception caught in running server: " << e.what() << std::endl;
	}
}

void preforked_server::start()
{
	m_lock.unlock();
}

bool preforked_server::read_socket_from_parent(int fd_socket, boost::asio::ip::tcp::socket& socket)
{
	typedef boost::asio::ip::tcp::socket::native_handle_type native_handle_type;

#if __APPLE__
	// macos is special...
	assert(CMSG_SPACE(sizeof(int)) == 16);
#endif

	struct msghdr	msg;
	union {
	  struct cmsghdr	cm;
#if __APPLE__
	  char				control[16];
#else
	  char				control[CMSG_SPACE(sizeof(int))];
#endif
	} control_un;

	msg.msg_control = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);
	msg.msg_name = nullptr;
	msg.msg_namelen = 0;

	boost::asio::ip::tcp::socket::endpoint_type peer_endpoint;

	struct iovec iov[1];
	iov[0].iov_base = peer_endpoint.data();
	iov[0].iov_len = peer_endpoint.capacity();
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	bool result = false;
	int n = recvmsg(fd_socket, &msg, 0);
	if (n >= 0)
	{
		peer_endpoint.resize(n);
	
		struct cmsghdr* cmptr CMSG_FIRSTHDR(&msg);
		if (cmptr != nullptr and cmptr->cmsg_len == CMSG_LEN(sizeof(int)))
		{
			if (cmptr->cmsg_level != SOL_SOCKET)
			 	std::cerr << "control level != SOL_SOCKET" << std::endl;
			else if (cmptr->cmsg_type != SCM_RIGHTS)
				std::cerr << "control type != SCM_RIGHTS";
			else
			{
				/* Produces warning: dereferencing type-punned pointer will break strict-aliasing rules [-Wstrict-aliasing]
				int fd = *(reinterpret_cast<native_handle_type*>(CMSG_DATA(cmptr)));
				*/
				native_handle_type *fdptr = reinterpret_cast<native_handle_type*>(CMSG_DATA(cmptr));
				int fd = *fdptr;
				if (fd >= 0)
				{
					socket.assign(peer_endpoint.protocol(), fd);
					result = true;
				}
			}
		}
	}
	
	return result;
}

void preforked_server::write_socket_to_worker(int fd_socket, boost::asio::ip::tcp::socket& socket)
{
	typedef boost::asio::ip::tcp::socket::native_handle_type native_handle_type;
	
	struct msghdr msg;
	union {
		struct cmsghdr	cm;
#if __APPLE__
	  char				control[16];
#else
	  char				control[CMSG_SPACE(sizeof(native_handle_type))];
#endif
	} control_un;
	
	msg.msg_control = control_un.control;
	msg.msg_controllen = sizeof(control_un.control);
	
	struct cmsghdr* cmptr = CMSG_FIRSTHDR(&msg);
	cmptr->cmsg_len = CMSG_LEN(sizeof(int));
	cmptr->cmsg_level = SOL_SOCKET;
	cmptr->cmsg_type = SCM_RIGHTS;
	/* Procudes warning: dereferencing type-punned pointer will break strict-aliasing rules [-Wstrict-aliasing]
	*(reinterpret_cast<native_handle_type*>(CMSG_DATA(cmptr))) = socket.native();
	*/
	native_handle_type *fdptr = reinterpret_cast<native_handle_type*>(CMSG_DATA(cmptr));
	*fdptr = socket.native_handle();
	
	msg.msg_name = nullptr;
	msg.msg_namelen = 0;
	
	struct iovec iov[1];
	iov[0].iov_base = socket.remote_endpoint().data();
	iov[0].iov_len = socket.remote_endpoint().size();
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	
	int err = sendmsg(fd_socket, &msg, 0);
	if (err < 0)
		throw exception("error passing filedescriptor: %s", strerror(errno));
}

void preforked_server::stop()
{
	close(m_fd);
	m_fd = -1;

	m_io_service.stop();
}

void preforked_server::handle_accept(const boost::system::error_code& ec)
{
	if (not ec)
	{
		try
		{
			write_socket_to_worker(m_fd, m_socket);
		}
		catch (const std::exception& e)
		{
			std::cerr << "error writing socket to client: " << e.what() << std::endl;
			
			reply r = reply::stock_reply(service_unavailable);

			std::vector<boost::asio::const_buffer> buffers;
			r.to_buffers(buffers);

			try
			{
				boost::asio::write(m_socket, buffers);
			}
			catch(const std::exception& e)
			{
				std::cerr << e.what() << std::endl;
			}
		}

		m_socket.close();

		m_acceptor.async_accept(m_socket,
			[this](boost::system::error_code ec) { handle_accept(ec); });
	}
}

}
}

#endif
