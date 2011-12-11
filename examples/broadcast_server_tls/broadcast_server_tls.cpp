/*
 * Copyright (c) 2011, Peter Thorson. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the WebSocket++ Project nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL PETER THORSON BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */

#include "../../src/endpoint.hpp"
#include "../../src/roles/server.hpp"
#include "../../src/sockets/ssl.hpp"

#include <boost/date_time/posix_time/posix_time.hpp>

#include <cstring>
#include <set>

#include <sys/resource.h>

typedef websocketpp::endpoint<websocketpp::role::server,websocketpp::socket::plain> plain_endpoint_type;
typedef plain_endpoint_type::handler_ptr plain_handler_ptr;

typedef websocketpp::endpoint<websocketpp::role::server,websocketpp::socket::ssl> tls_endpoint_type;
typedef tls_endpoint_type::handler_ptr tls_handler_ptr;

template <typename endpoint_type>
class broadcast_server_handler : public endpoint_type::handler {
public:
	typedef broadcast_server_handler<endpoint_type> type;
	typedef typename endpoint_type::connection_ptr connection_ptr;
    
	broadcast_server_handler() 
     : m_epoch(boost::posix_time::time_from_string("1970-01-01 00:00:00.000")) {
		m_messages = 0;
		m_data = 0;
	}
	
	std::string get_password() const {
		return "test";
	}
	
	boost::shared_ptr<boost::asio::ssl::context> on_tls_init() {
		// create a tls context, init, and return.
		boost::shared_ptr<boost::asio::ssl::context> context(new boost::asio::ssl::context(boost::asio::ssl::context::tlsv1));
		try {
			context->set_options(boost::asio::ssl::context::default_workarounds |
								 boost::asio::ssl::context::no_sslv2 |
								 boost::asio::ssl::context::single_dh_use);
			context->set_password_callback(boost::bind(&type::get_password, this));
			context->use_certificate_chain_file("../../src/ssl/server.pem");
			context->use_private_key_file("../../src/ssl/server.pem", boost::asio::ssl::context::pem);
			context->use_tmp_dh_file("../../src/ssl/dh512.pem");
		} catch (std::exception& e) {
			std::cout << e.what() << std::endl;
		}
		return context;
	}
	
	void validate(connection_ptr connection) {
		//std::cout << "state: " << connection->get_state() << std::endl;
	}
	
	void on_open(connection_ptr connection) {
		if (!m_timer) {
			m_timer.reset(new boost::asio::deadline_timer(connection->get_io_service(),boost::posix_time::seconds(0)));
			m_timer->expires_from_now(boost::posix_time::milliseconds(1000));
			m_timer->async_wait(boost::bind(&type::on_timer,this,boost::asio::placeholders::error));
			m_last_time = boost::posix_time::microsec_clock::local_time();
		}
		
		if (connection->get_resource() == "/admin") {
			m_admin_connections.insert(connection);
		} else {
			m_connections.insert(connection);
		}
		
		typename std::set<connection_ptr>::iterator it;
		
		std::stringstream foo;
        foo << "{\"type\":\"con\""
            << ",\"timestamp\":" << get_ms()
            << ",\"value\":" << m_connections.size()
            << "}";
		
		for (it = m_admin_connections.begin(); it != m_admin_connections.end(); it++) {
			(*it)->send(foo.str(),false);
		}
	}
	
	void on_close(connection_ptr connection) {
		//std::cout << "connection closed" << std::endl;
		m_connections.erase(connection);
		m_admin_connections.erase(connection);
		
		typename std::set<connection_ptr>::iterator it;
		
		std::stringstream foo;
        foo << "{\"type\":\"con\""
            << ",\"timestamp\":" << get_ms()
            << ",\"value\":" << m_connections.size()
            << "}";
        
		for (it = m_admin_connections.begin(); it != m_admin_connections.end(); it++) {
			(*it)->send(foo.str(),false);
		}
		
	}
	
	void on_message(connection_ptr connection,websocketpp::message::data_ptr msg) {
		typename std::set<connection_ptr>::iterator it;
		
		
		
		// broadcast to clients
		for (it = m_connections.begin(); it != m_connections.end(); it++) {
			m_messages++;
			m_data += msg->get_payload().size();
			(*it)->send(msg->get_payload(),(msg->get_opcode() == websocketpp::frame::opcode::BINARY));
		}
		
		// broadcast to admins
		std::stringstream foo;
		foo << "{\"type\":\"message\",\"value\":\""; 
		
		if (msg->get_opcode() == websocketpp::frame::opcode::BINARY) {
			foo << "[Binary Message, length: " << msg->get_payload().size() << "]";
		} else {
            if (msg->get_payload().size() > 126) {
                foo << "[UTF8 Message, length: " << msg->get_payload().size() << "]";
            } else {
                foo << msg->get_payload();
            }
		}
		
		foo << "\"}";
		
		for (it = m_admin_connections.begin(); it != m_admin_connections.end(); it++) {
			m_messages++;
			m_data += msg->get_payload().size();
			(*it)->send(foo.str(),false);
		}
		
		connection->recycle(msg);
	}
	
	void http(connection_ptr connection) {
		std::stringstream foo;
		
		foo << "<html><body><p>" << m_connections.size() << " current connections.</p></body></html>";
		
		connection->set_body(foo.str());
	}
	
    long get_ms() {
        boost::posix_time::ptime now = boost::posix_time::microsec_clock::local_time();
        boost::posix_time::time_period period(m_epoch,now);
        return period.length().total_milliseconds();
    }
    
	void on_timer(const boost::system::error_code& error) {
		// there is new data. This is the first time that there is no new data
		
		//if (m_messages != m_messages_cache || m_data != m_data_cache) {
			
			//boost::posix_time::time_period period(m_last_time,now);
			//m_last_time = now;
            
			
			long milli_seconds = get_ms();
			
			//double seconds = milli_seconds/1000.0;
			
			m_messages_cache = m_messages;
			m_data_cache = m_data;
			
			m_messages_sent += m_messages;
			m_data_sent += m_data;
			
			//std::cout << "m: " << m_messages 
			//		  << " milli: " << milli_seconds 
			//		  << std::endl;
			
			std::stringstream foo;
			foo << "{\"type\":\"stats\""
                << ",\"timestamp\":" << milli_seconds
                << ",\"messages\":" << m_messages
                << ",\"bytes\":" << m_data
                << ",\"messages_sent\":" << m_messages_sent
                << ",\"bytes_sent\":" << m_data_sent
                << ",\"connections\":" << m_connections.size()
                << ",\"admin_connections\":" << m_admin_connections.size()
                << "}";
            
				//<< ((m_messages_cache * seconds)*1000) << ",\"data\":" 
				//<< ((m_data_cache * seconds)*1000) << ",\"messages_sent\":" 
				//<< m_messages_sent <<",\"data_sent\":" << m_data_sent << "}";
			
			typename std::set<connection_ptr>::iterator it;
			
			for (it = m_admin_connections.begin(); it != m_admin_connections.end(); it++) {
				(*it)->send(foo.str(),false);
			}
			
			m_messages = 0;
			m_data = 0;
		//}
		
		m_timer->expires_from_now(boost::posix_time::milliseconds(1000));
		m_timer->async_wait(boost::bind(&type::on_timer,this,boost::asio::placeholders::error));
	}
	
	void on_fail(connection_ptr connection) {
		std::cout << "connection failed" << std::endl;
	}
private:
	unsigned int				m_messages;
	size_t						m_data;
	unsigned int				m_messages_cache;
	size_t						m_data_cache;
	unsigned int				m_messages_sent;
	size_t						m_data_sent;
	boost::shared_ptr<boost::asio::deadline_timer> m_timer;
	boost::posix_time::ptime	m_epoch;
    boost::posix_time::ptime	m_last_time;
	
	std::set<connection_ptr>	m_connections;
	std::set<connection_ptr>	m_admin_connections;
};

int main(int argc, char* argv[]) {
	unsigned short port = 9002;
	bool tls = false;
    
    // 12288 is max OS X limit without changing kernal settings
    const rlim_t ideal_size = 100000;
    rlim_t old_size;
    
    struct rlimit rl;
    int result;
    
    result = getrlimit(RLIMIT_NOFILE, &rl);
    if (result == 0) {
        std::cout << "cur: " << rl.rlim_cur << " max: " << rl.rlim_max << std::endl;
        
        old_size = rl.rlim_cur;
        
        if (rl.rlim_cur < ideal_size) {
            rl.rlim_cur = ideal_size;
            //rl.rlim_cur = rl.rlim_max;
            result = setrlimit(RLIMIT_NOFILE, &rl);
            
            if (result != 0) {
                std::cout << "Unable to request an increase in the file descripter limit. This server will be limited to " << old_size << " concurrent connections. Error code: " << errno << std::endl;
            }
        }
    }
    
	if (argc == 2) {
		// TODO: input validation?
		port = atoi(argv[1]);
	}
	
	if (argc == 3) {
		// TODO: input validation?
		port = atoi(argv[1]);
		tls = !strcmp(argv[2],"-tls");
	}
	
	try {
		if (tls) {
			tls_handler_ptr h(new broadcast_server_handler<tls_endpoint_type>());
			tls_endpoint_type e(h);
			
			e.alog().unset_level(websocketpp::log::alevel::ALL);
			e.elog().set_level(websocketpp::log::elevel::ALL);
			
			std::cout << "Starting Secure WebSocket broadcast server on port " << port << std::endl;
			e.listen(port);
		} else {
			plain_handler_ptr h(new broadcast_server_handler<plain_endpoint_type>());
			plain_endpoint_type e(h);
			
			e.alog().unset_level(websocketpp::log::alevel::ALL);
			e.elog().set_level(websocketpp::log::elevel::ALL);
			
			std::cout << "Starting WebSocket broadcast server on port " << port << std::endl;
			e.listen(port);
		}
	} catch (std::string e) {
		//std::cerr << "Exception: " << e.what() << std::endl;
		
	}
	
	return 0;
}