#include "common.h"
#include "log.h"
#include "git_version.h"
#include "fd_manager.h"
#define DOMAIN_MAX_LEN 64
// 跨平台多进程支持
#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/wait.h>
#endif

using  namespace std;

typedef unsigned long long u64_t;   //this works on most platform,avoid using the PRId64
typedef long long i64_t;

typedef unsigned int u32_t;
typedef int i32_t;

int disable_conn_clear=0;

int max_pending_packet=0;
int enable_udp=0,enable_tcp=0;

const int listen_fd_buf_size=2*1024*1024;
address_t local_addr,remote_addr;

int VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV;

//template <class key_t>
struct lru_collector_t:not_copy_able_t
{
	typedef void* key_t;
//#define key_t void*
	struct lru_pair_t
	{
		key_t key;
		my_time_t ts;
	};
	unordered_map<key_t,list<lru_pair_t>::iterator> mp;
	list<lru_pair_t> q;
	int update(key_t key)
	{
		assert(mp.find(key)!=mp.end());
		auto it=mp[key];
		q.erase(it);

		my_time_t value=get_current_time();
		if(!q.empty())
		{
			assert(value >=q.front().ts);
		}
		lru_pair_t tmp; tmp.key=key; tmp.ts=value;
		q.push_front( tmp);
		mp[key]=q.begin();

		return 0;
	}
	int new_key(key_t key)
	{
		assert(mp.find(key)==mp.end());

		my_time_t value=get_current_time();
		if(!q.empty())
		{
			assert(value >=q.front().ts);
		}
		lru_pair_t tmp; tmp.key=key; tmp.ts=value;
		q.push_front( tmp);
		mp[key]=q.begin();

		return 0;
	}
	int size()
	{
		return q.size();
	}
	int empty()
	{
		return q.empty();
	}
	void clear()
	{
		mp.clear(); q.clear();
	}
	my_time_t ts_of(key_t key)
	{
		assert(mp.find(key)!=mp.end());
		return mp[key]->ts;
	}

	my_time_t peek_back(key_t &key)
	{
		assert(!q.empty());
		auto it=q.end(); it--;
		key=it->key;
		return it->ts;
	}
	void erase(key_t key)
	{
		assert(mp.find(key)!=mp.end());
		q.erase(mp[key]);
		mp.erase(key);
	}
	/*
	void erase_back()
	{
		assert(!q.empty());
		auto it=q.end(); it--;
		key_t key=it->key;
		erase(key);
	}*/
};

struct conn_manager_udp_t
{
	unordered_map<address_t,udp_pair_t*,address_t::hash_function> adress_to_info;

	list<udp_pair_t> udp_pair_list;
	long long last_clear_time;
	lru_collector_t lru;
	//list<udp_pair_t>::iterator clear_it;

	conn_manager_udp_t()
	{
		last_clear_time=0;
		adress_to_info.reserve(10007);
		//clear_it=udp_pair_list.begin();
	}

	int erase(list<udp_pair_t>::iterator &it)
	{
		struct ev_loop * loop= ev_default_loop (0);
		ev_io_stop(loop, &it->ev);

		mylog(log_info,"[udp]inactive connection {%s} cleared, udp connections=%d\n",it->addr_s,(int)(udp_pair_list.size()-1));
		mylog(log_debug,"[udp] lru.size()=%d\n",(int)lru.size()-1);

		auto tmp_it=adress_to_info.find(it->adress);
		assert(tmp_it!=adress_to_info.end());
		adress_to_info.erase(tmp_it);

		fd_manager.fd64_close(it->fd64);
		lru.erase(&*it);
		udp_pair_list.erase(it);

		return 0;
	}

	int clear_inactive()
	{
		if(get_current_time()-last_clear_time>conn_clear_interval)
		{
			last_clear_time=get_current_time();
			return clear_inactive0();
		}
		return 0;
	}
	int clear_inactive0()
	{
		if(disable_conn_clear) return 0;

		int cnt=0;
		//list<tcp_pair_t>::iterator it=clear_it,old_it;
		int size=udp_pair_list.size();
		int num_to_clean=size/conn_clear_ratio+conn_clear_min;   //clear 2% each time,to avoid latency glitch

		u64_t current_time=get_current_time();
		num_to_clean=min(num_to_clean,size);
		for(;;)
		{
			if(cnt>=num_to_clean) break;
			//if(tcp_pair_list.begin()==tcp_pair_list.end()) break;
			if(lru.empty()) break;
			lru_collector_t::key_t key;
			my_time_t ts=lru.peek_back(key);
			if(current_time- ts < conn_timeout_tcp) break;

			erase( ((udp_pair_t *) key)->it  );

			cnt++;
		}
		return 0;
	}
}conn_manager_udp;

struct conn_manager_tcp_t
{
	list<tcp_pair_t> tcp_pair_list;
	long long last_clear_time;
	lru_collector_t lru;
	conn_manager_tcp_t()
	{
		last_clear_time=0;
	}
	int erase(list<tcp_pair_t>::iterator &it)
	{
		struct ev_loop * loop= ev_default_loop (0);
		ev_io_stop(loop, &it->local.ev);
		ev_io_stop(loop, &it->remote.ev);

		fd_manager.fd64_close( it->local.fd64);
		fd_manager.fd64_close( it->remote.fd64);
		mylog(log_info,"[tcp]inactive connection {%s} cleared, tcp connections=%d\n",it->addr_s,(int)(tcp_pair_list.size()-1));
		mylog(log_debug,"[tcp] lru.size()=%d\n",(int)lru.size()-1);
		lru.erase(&*it);
		tcp_pair_list.erase(it);
		return 0;
	}
	int erase_closed(list<tcp_pair_t>::iterator &it)//just a copy of erase()
	{
		struct ev_loop * loop= ev_default_loop (0);
		ev_io_stop(loop, &it->local.ev);
		ev_io_stop(loop, &it->remote.ev);

		fd_manager.fd64_close( it->local.fd64);
		fd_manager.fd64_close( it->remote.fd64);
		mylog(log_info,"[tcp]closed connection {%s} cleared, tcp connections=%d\n",it->addr_s,(int)(tcp_pair_list.size()-1));
		mylog(log_debug,"[tcp] lru.size()=%d\n",(int)lru.size()-1);
		lru.erase(&*it);
		tcp_pair_list.erase(it);
		return 0;
	}
	int clear_inactive()
	{
		if(get_current_time()-last_clear_time>conn_clear_interval)
		{
			last_clear_time=get_current_time();
			return clear_inactive0();
		}
		return 0;
	}
	int clear_inactive0()
	{

		if(disable_conn_clear) return 0;

		int cnt=0;
		//list<tcp_pair_t>::iterator it=clear_it,old_it;
		int size=tcp_pair_list.size();
		int num_to_clean=size/conn_clear_ratio+conn_clear_min;   //clear 2% each time,to avoid latency glitch

		u64_t current_time=get_current_time();
		num_to_clean=min(num_to_clean,size);
		for(;;)
		{
			if(cnt>=num_to_clean) break;
			//if(tcp_pair_list.begin()==tcp_pair_list.end()) break;
			if(lru.empty()) break;
			lru_collector_t::key_t key;
			my_time_t ts=lru.peek_back(key);
			if(current_time- ts < conn_timeout_tcp) break;

			erase( ((tcp_pair_t *) key)->it  );

			cnt++;
		}
		//clear_it=it;

		return 0;
	}
}conn_manager_tcp;

void tcp_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
	if((revents&EV_ERROR) !=0)
	{
		assert(0==1);
	}
	//mylog(log_info,"[tcp]tcp_cb called \n");
	fd64_t fd64=watcher->u64;
	if(!fd_manager.exist(fd64))
	{
		mylog(log_warn,"[tcp]fd64 no longer exist\n");
		return;
	}


	assert(fd_manager.exist_info(fd64));
	fd_info_t & fd_info=fd_manager.get_info(fd64);

	assert(fd_info.is_tcp==1);

	tcp_pair_t &tcp_pair=*(fd_info.tcp_pair_p);

	/*if((revents&EV_ERROR) !=0)
	{
		assert(0==1);
		//mylog(log_info,"[tcp]connection closed, events[idx].events=%x \n",(u32_t) revents);
		//ev_io_stop(loop, &tcp_pair.local.ev);
		//ev_io_stop(loop, &tcp_pair.remote.ev);
		//conn_manager_tcp.erase(tcp_pair.it);
		return;
	}*/

	tcp_info_t *my_info_p,*other_info_p;
	if(fd64==tcp_pair.local.fd64)
	{
		mylog(log_trace,"[tcp]fd64==tcp_pair.local.fd64\n");
		my_info_p=&tcp_pair.local;
		other_info_p=&tcp_pair.remote;
	}
	else if(fd64==tcp_pair.remote.fd64)
	{
		mylog(log_trace,"[tcp]fd64==tcp_pair.remote.fd64\n");
		my_info_p=&tcp_pair.remote;
		other_info_p=&tcp_pair.local;
	}
	else
	{
		assert(0==1);
	}
	tcp_info_t &my_info=*my_info_p;
	tcp_info_t &other_info=*other_info_p;

	int my_fd=fd_manager.to_fd(my_info.fd64);
	int other_fd=fd_manager.to_fd(other_info.fd64);

	assert(watcher->fd==my_fd);

	//zdmylog(log_info,"[tcp]my_fd=%d,other_fd=%d \n",my_fd,other_fd);

	if( (revents & EV_READ) !=0  )
	{
		mylog(log_trace,"[tcp]events[idx].events & EPOLLIN !=0 \n");
		if((my_info.ev.events&EV_READ) ==0)
		{
			mylog(log_debug,"[tcp]out of date event, my_info.ev.events&EPOLLIN) ==0 \n");
			return;
		}
		assert(my_info.data_len==0);
		int recv_len=recv(my_fd,my_info.data,max_data_len_tcp,0);//use a larger buffer than udp
		mylog(log_trace,"fd=%d,recv_len=%d\n",my_fd,recv_len);
		if(recv_len==0)
		{
			mylog(log_info,"[tcp]recv_len=%d,connection {%s} closed bc of EOF\n",recv_len,tcp_pair.addr_s);
			conn_manager_tcp.erase_closed(tcp_pair.it);
			return;
		}
		if(recv_len<0)
		{
			mylog(log_info,"[tcp]recv_len=%d,connection {%s} closed bc of %s,fd=%d\n",recv_len,tcp_pair.addr_s,get_sock_error(),my_fd);
			conn_manager_tcp.erase_closed(tcp_pair.it);
			return;
		}
		conn_manager_tcp.lru.update(&(*tcp_pair.it));
		//tcp_pair.last_active_time=get_current_time();

		my_info.data_len=recv_len;
		my_info.begin=my_info.data;

		assert((other_info.ev.events & EV_WRITE)==0);

		int send_len=send(other_fd,my_info.begin,my_info.data_len,0);

		if(send_len<=0)
		{
			//NOP
		}
		else
		{
			my_info.data_len-=send_len;
			my_info.begin+=send_len;
		}

		if(my_info.data_len!=0)
		{
			//epoll_event ev;

			//ev=other_info.ev;
			//ret = epoll_ctl(epollfd, EPOLL_CTL_MOD, other_fd, &ev);
			//assert(ret==0);
			ev_io_stop(loop, &other_info.ev);
			other_info.ev.events|=EV_WRITE;
			ev_io_init (&other_info.ev, tcp_cb, other_fd, other_info.ev.events);
			ev_io_start(loop, &other_info.ev);


			//ev=my_info.ev;
			//ret = epoll_ctl(epollfd, EPOLL_CTL_MOD, my_fd, &ev);
			//assert(ret==0);
			ev_io_stop(loop, &my_info.ev);
			my_info.ev.events&=~EV_READ;
			ev_io_init (&my_info.ev, tcp_cb, my_fd, my_info.ev.events);
			ev_io_start(loop, &my_info.ev);

		}

	}
	else if( (revents & EV_WRITE) !=0)
	{
		mylog(log_trace,"[tcp]events[idx].events & EPOLLOUT !=0\n");

		if( (my_info.ev.events&EV_WRITE) ==0)
		{
			mylog(log_debug,"[tcp]out of date event, my_info.ev.events&EPOLLOUT) ==0 \n");
			return;
		}

		assert(other_info.data_len!=0);
		int send_len=send(my_fd,other_info.begin,other_info.data_len,0);
		if(send_len==0)
		{
			mylog(log_warn,"[tcp]send_len=%d,connection {%s} closed bc of send_len==0\n",send_len,tcp_pair.addr_s);
			conn_manager_tcp.erase_closed(tcp_pair.it);
			return;
		}
		if(send_len<0)
		{
			mylog(log_info,"[tcp]send_len=%d,connection {%s} closed bc of %s\n",send_len,tcp_pair.addr_s,get_sock_error());
			conn_manager_tcp.erase_closed(tcp_pair.it);
			return;
		}
		conn_manager_tcp.lru.update(&(*tcp_pair.it));

		//tcp_pair.last_active_time=get_current_time();

		mylog(log_trace,"[tcp]fd=%d send len=%d\n",my_fd,send_len);
		other_info.data_len-=send_len;
		other_info.begin+=send_len;

		if(other_info.data_len==0)
		{

			//ev=my_info.ev;
			//ret = epoll_ctl(epollfd, EPOLL_CTL_MOD, my_fd, &ev);
			//assert(ret==0);
			ev_io_stop(loop, &my_info.ev);
			my_info.ev.events&=~EV_WRITE;
			ev_io_init (&my_info.ev, tcp_cb, my_fd, my_info.ev.events);
			ev_io_start(loop, &my_info.ev);

			assert((other_info.ev.events & EV_READ)==0);



			//ev=other_info.ev;
			//ret = epoll_ctl(epollfd, EPOLL_CTL_MOD, other_fd, &ev);
			//assert(ret==0);
			ev_io_stop(loop, &other_info.ev);
			other_info.ev.events|=EV_READ;
			ev_io_init (&other_info.ev, tcp_cb, other_fd, other_info.ev.events);
			ev_io_start(loop, &other_info.ev);
		}
		else
		{
			//keep waitting for EPOLLOUT;
		}
	}
	else
	{
		mylog(log_fatal,"[tcp]got unexpected event,events[idx].events=%x\n",(u32_t)revents);
		myexit(-1);
	}
}
void tcp_accept_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
	int ret;
	int local_listen_fd_tcp=watcher->fd;
	if((revents&EV_ERROR) !=0)
	{
		assert(0==1);
		return ;
	}
	/*if((events[idx].events & EPOLLERR) !=0 ||(events[idx].events & EPOLLHUP) !=0)
	{
		mylog(log_error,"[tcp]EPOLLERR or EPOLLHUP from listen_fd events[idx].events=%x \n",events[idx].events);
		//if there is an error, we will eventually get it at accept()
	}*/

	socklen_t tmp_len = sizeof(address_t::storage_t);
	address_t::storage_t tmp_sockaddr_in={0};
	memset(&tmp_sockaddr_in,0,sizeof(tmp_sockaddr_in));

	int new_fd=accept(local_listen_fd_tcp, (struct sockaddr*) &tmp_sockaddr_in,&tmp_len);
	if(new_fd<0)
	{
		mylog(log_warn,"[tcp]accept failed %d %s\n", new_fd,get_sock_error());
		//continue;
		return ;
	}

	address_t tmp_addr;
	tmp_addr.from_sockaddr((sockaddr*)&tmp_sockaddr_in,tmp_len);

	set_buf_size(new_fd,socket_buf_size);
	setnonblocking(new_fd);

	char ip_addr[max_addr_len];
	tmp_addr.to_str(ip_addr);
	//sprintf(ip_port_s,"%s:%d",my_ntoa(addr_tmp.sin_addr.s_addr),addr_tmp.sin_port);

	if(int(conn_manager_tcp.tcp_pair_list.size())>=max_conn_num)
	{
		mylog(log_warn,"[tcp]new connection from {%s},but ignored,bc of max_conn_num reached\n",ip_addr);
		sock_close(new_fd);
		return ;
		//continue;
	}


	int new_remote_fd = socket(remote_addr.get_type(), SOCK_STREAM, 0);
	if(new_remote_fd<0)
	{
		mylog(log_fatal,"[tcp]create new_remote_fd failed \n");
		myexit(1);
	}
	set_buf_size(new_remote_fd,socket_buf_size);
	setnonblocking(new_remote_fd);

	ret=connect(new_remote_fd,(struct sockaddr*) &remote_addr.inner,remote_addr.get_len());
	if(ret!=0)
	{
		mylog(log_debug,"[tcp]connect returned %d,errno=%s\n",ret,get_sock_error());
	}
	else
	{
		mylog(log_debug,"[tcp]connect returned 0\n");
	}

	conn_manager_tcp.tcp_pair_list.emplace_back();
	auto it=conn_manager_tcp.tcp_pair_list.end();
	it--;

	conn_manager_tcp.lru.new_key(&(*it));

	tcp_pair_t &tcp_pair=*it;
	strcpy(tcp_pair.addr_s,ip_addr);

	mylog(log_info,"[tcp]new_connection from {%s},fd1=%d,fd2=%d,tcp connections=%d\n",tcp_pair.addr_s,new_fd,new_remote_fd,(int)conn_manager_tcp.tcp_pair_list.size());

	tcp_pair.local.fd64=fd_manager.create(new_fd);
	fd_manager.get_info(tcp_pair.local.fd64).tcp_pair_p= &tcp_pair;
	fd_manager.get_info(tcp_pair.local.fd64).is_tcp=1;
	//tcp_pair.local.ev.events=EV_READ;
	tcp_pair.local.ev.u64=tcp_pair.local.fd64;

	tcp_pair.remote.fd64=fd_manager.create(new_remote_fd);
	fd_manager.get_info(tcp_pair.remote.fd64).tcp_pair_p= &tcp_pair;
	fd_manager.get_info(tcp_pair.remote.fd64).is_tcp=1;
	//tcp_pair.remote.ev.events=EV_READ;
	tcp_pair.remote.ev.u64=tcp_pair.remote.fd64;

	conn_manager_tcp.lru.update(&(*it));
	//tcp_pair.last_active_time=get_current_time();
	tcp_pair.it=it;

	//epoll_event ev;

	//ev=tcp_pair.local.ev;
	//ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, new_fd, &ev);
	//assert(ret==0);
	ev_io_init (&tcp_pair.local.ev, tcp_cb, new_fd, EV_READ);
	ev_io_start(loop, &tcp_pair.local.ev);

	//ev=tcp_pair.remote.ev;
	//ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, new_remote_fd, &ev);
	//assert(ret==0);

	ev_io_init (&tcp_pair.remote.ev, tcp_cb, new_remote_fd, EV_READ);
	ev_io_start(loop, &tcp_pair.remote.ev);
}

void clear_timer_cb(struct ev_loop *loop, struct ev_timer* timer, int revents)
{
	conn_manager_tcp.clear_inactive();
	conn_manager_udp.clear_inactive();
}
void udp_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{
	if((revents&EV_ERROR) !=0)
	{
		assert(0==1);
	}
	int ret;
	fd64_t fd64=watcher->u64;
	if(!fd_manager.exist(fd64))
	{
		mylog(log_warn,"[udp]fd64 no longer exist\n");
		return;
	}
	assert(fd_manager.exist_info(fd64));
	fd_info_t & fd_info=fd_manager.get_info(fd64);
	assert(fd_info.is_tcp==0);

	int udp_fd=fd_manager.to_fd(fd64);
	udp_pair_t & udp_pair=*fd_manager.get_info(fd64).udp_pair_p;
	//assert(conn_manager.exist_fd(udp_fd));
	//if(!conn_manager.exist_fd(udp_fd)) continue;

	/*
	if((events[idx].events & EPOLLERR) !=0 ||(events[idx].events & EPOLLHUP) !=0)
	{
		mylog(log_warn,"[udp]EPOLLERR or EPOLLHUP from udp_remote_fd events[idx].events=%x \n",events[idx].events);

	}*/

	char data[max_data_len_udp+200];
	int data_len =recv(udp_fd,data,max_data_len_udp+1,0);
	mylog(log_trace, "[udp]received data from udp fd %d, len=%d\n", udp_fd,data_len);

	if(data_len==max_data_len_udp+1)
	{
		mylog(log_warn,"huge packet from {%s}, data_len > %d,dropped\n",udp_pair.addr_s,max_data_len_udp);
		return;
	}

	if(data_len<0)
	{
		mylog(log_warn,"[udp]recv failed %d ,udp_fd%d,errno:%s\n", data_len,udp_fd,get_sock_error());
		return;
	}

	conn_manager_udp.lru.update(&udp_pair);
	//udp_pair.last_active_time=get_current_time();
	int local_listen_fd_udp=udp_pair.local_listen_fd;
	ret = sendto(local_listen_fd_udp, data,data_len,0, (struct sockaddr *)&udp_pair.adress.inner,udp_pair.adress.get_len());
	if (ret < 0) {
		mylog(log_warn, "[udp]sento returned %d,%s\n", ret,get_sock_error());
		//perror("ret<0");
	}
}
void udp_accept_cb(struct ev_loop *loop, struct ev_io *watcher, int revents)
{

	if((revents&EV_ERROR) !=0)
	{
		assert(0==1);
		return ;
	}
	/*if((events[idx].events & EPOLLERR) !=0 ||(events[idx].events & EPOLLHUP) !=0)
	{
		mylog(log_error,"[udp]EPOLLERR or EPOLLHUP from listen_fd events[idx].events=%x \n",events[idx].events);
		//if there is an error, we will eventually get it at recvfrom();
	}*/

	char data[max_data_len_udp+200];
	int data_len;

	socklen_t tmp_len = sizeof(address_t::storage_t);
	address_t::storage_t tmp_sockaddr_in={0};
	memset(&tmp_sockaddr_in,0,sizeof(tmp_sockaddr_in));

	int local_listen_fd_udp=watcher->fd;

	if ((data_len = recvfrom(local_listen_fd_udp, data, max_data_len_udp+1, 0,
			(struct sockaddr *) &tmp_sockaddr_in, &tmp_len)) == -1) //<--first packet from a new ip:port turple
	{
		mylog(log_debug,"[udp]recv_from error,errno %s,this shouldnt happen,but lets try to pretend it didnt happen",get_sock_error());
		//myexit(1);
		return;
	}

	address_t tmp_addr;
	tmp_addr.from_sockaddr((sockaddr*)&tmp_sockaddr_in,tmp_len);

	data[data_len] = 0; //for easier debug

	char ip_addr[max_addr_len];
	tmp_addr.to_str(ip_addr);

	mylog(log_trace, "[udp]received data from udp_listen_fd from {%s}, len=%d\n",ip_addr,data_len);

	if(data_len==max_data_len_udp+1)
	{
		mylog(log_warn,"huge packet from {%s}, data_len > %d,dropped\n",ip_addr,max_data_len_udp);
		return;
	}

	auto it=conn_manager_udp.adress_to_info.find(tmp_addr);
	if(it==conn_manager_udp.adress_to_info.end())
	{

		if(int(conn_manager_udp.udp_pair_list.size())>=max_conn_num)
		{
			mylog(log_info,"[udp]new connection from {%s},but ignored,bc of max_conv_num reached\n",ip_addr);
			return;
		}
		int new_udp_fd=remote_addr.new_connected_udp_fd();
		if(new_udp_fd==-1)
		{
			mylog(log_info,"[udp]new connection from {%s} ,but create udp fd failed\n",ip_addr);
			return;
		}
		fd64_t fd64=fd_manager.create(new_udp_fd);
		fd_manager.get_info(fd64);//just create the info

		//struct epoll_event ev;
		mylog(log_trace, "[udp]u64: %lld\n", fd64);
		//ev.events = EPOLLIN;
		//ev.data.u64 = fd64;

		//ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, new_udp_fd, &ev);
		//assert(ret==0);


		conn_manager_udp.udp_pair_list.emplace_back();
		auto list_it=conn_manager_udp.udp_pair_list.end();
		list_it--;
		conn_manager_udp.lru.new_key(&(*list_it));
		udp_pair_t &udp_pair=*list_it;

		udp_pair.ev.u64=fd64;
		ev_io_init (&udp_pair.ev, udp_cb, new_udp_fd, EV_READ);
		ev_io_start(loop, &udp_pair.ev);


		mylog(log_info,"[udp]new connection from {%s},udp fd=%d,udp connections=%d\n",ip_addr,new_udp_fd,(int)conn_manager_udp.udp_pair_list.size());

		udp_pair.adress=tmp_addr;
		udp_pair.fd64=fd64;
		//udp_pair.last_active_time=get_current_time();
		strcpy(udp_pair.addr_s,ip_addr);
		udp_pair.it=list_it;
		udp_pair.local_listen_fd=local_listen_fd_udp;

		fd_manager.get_info(fd64).udp_pair_p=&udp_pair;
		conn_manager_udp.adress_to_info[tmp_addr]=&udp_pair;
		it=conn_manager_udp.adress_to_info.find(tmp_addr);
		//it=adress_to_info.
	}

	//auto it=conn_manager_udp.adress_to_info.find(tmp_addr);
	assert(it!=conn_manager_udp.adress_to_info.end() );

	udp_pair_t &udp_pair=*(it->second);
	int udp_fd= fd_manager.to_fd(udp_pair.fd64);
	conn_manager_udp.lru.update(&udp_pair);
	//udp_pair.last_active_time=get_current_time();

	int ret;
	ret = send(udp_fd, data,data_len, 0);
	if (ret < 0) {
		mylog(log_warn, "[udp]send returned %d,%s\n", ret,get_sock_error() );
	}

}
void sigpipe_cb(struct ev_loop *l, ev_signal *w, int revents)
{
	mylog(log_info, "got sigpipe, ignored");
}

void sigterm_cb(struct ev_loop *l, ev_signal *w, int revents)
{
	mylog(log_info, "got sigterm, exit");
	myexit(0);
}

void sigint_cb(struct ev_loop *l, ev_signal *w, int revents)
{
	mylog(log_info, "got sigint, exit");
	myexit(0);
}


int event_loop()
{


	int local_listen_fd_tcp=-1;
	int local_listen_fd_udp=-1;

	//struct sockaddr_in local_me,remote_dst;
	int yes = 1;int ret;
	local_listen_fd_tcp = socket(local_addr.get_type(), SOCK_STREAM, 0);
	if(local_listen_fd_tcp<0)
	{
		mylog(log_fatal,"[tcp]create listen socket failed\n");
		myexit(1);
	}

	setsockopt(local_listen_fd_tcp, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); //avoid annoying bind problem
	set_buf_size(local_listen_fd_tcp,listen_fd_buf_size);
	setnonblocking(local_listen_fd_tcp);




	local_listen_fd_udp = socket(local_addr.get_type(), SOCK_DGRAM, IPPROTO_UDP);
	if(local_listen_fd_udp<0)
	{
		mylog(log_fatal,"[udp]create listen socket failed\n");
		myexit(1);
	}
	setsockopt(local_listen_fd_udp, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));  //this is not necessary.
	set_buf_size(local_listen_fd_udp,listen_fd_buf_size);
	setnonblocking(local_listen_fd_udp);


	//int epollfd = epoll_create1(0);
	//const int max_events = 4096;
	//struct epoll_event ev, events[max_events];
	//if (epollfd < 0)
	//{
	//	mylog(log_fatal,"epoll created return %d\n", epollfd);
	//	myexit(-1);
	//}

	struct ev_loop * loop= ev_default_loop(0);
	assert(loop != NULL);

	struct ev_io tcp_accept_watcher;

	if(enable_tcp)
	{
		if (::bind(local_listen_fd_tcp, (struct sockaddr*) &local_addr.inner, local_addr.get_len()) !=0)
		{
			mylog(log_fatal,"[tcp]socket bind failed, %s",get_sock_error());
			myexit(1);
		}

	    if (listen (local_listen_fd_tcp, 512) !=0) //512 is max pending tcp connection,its large enough
	    {
			mylog(log_fatal,"[tcp]socket listen failed error, %s",get_sock_error());
			myexit(1);
	    }

		//ev.events = EPOLLIN;
		//ev.data.u64 = local_listen_fd_tcp;
		//int ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, local_listen_fd_tcp, &ev);
		//if(ret!=0)
		//{
		//	mylog(log_fatal,"[tcp]epoll EPOLL_CTL_ADD return %d\n", epollfd);
		//	myexit(-1);
		//}
	    ev_io_init(&tcp_accept_watcher, tcp_accept_cb, local_listen_fd_tcp, EV_READ);
	    ev_io_start(loop, &tcp_accept_watcher);
	}

	struct ev_io udp_accept_watcher;

	if(enable_udp)
	{
		if (::bind(local_listen_fd_udp, (struct sockaddr*) &local_addr.inner, local_addr.get_len()) == -1)
		{
			mylog(log_fatal,"[udp]socket bind error");
			myexit(1);
		}

		//ev.events = EPOLLIN;
		//ev.data.u64 = local_listen_fd_udp;
		//int ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, local_listen_fd_udp, &ev);
		//if(ret!=0)
		//{
		//	mylog(log_fatal,"[udp]epoll created return %d\n", epollfd);
		//	myexit(-1);
		//}

	    ev_io_init(&udp_accept_watcher, udp_accept_cb, local_listen_fd_udp, EV_READ);
	    ev_io_start(loop, &udp_accept_watcher);
	}

	//int clear_timer_fd=-1;
	struct ev_timer clear_timer;

	ev_timer_init(&clear_timer, clear_timer_cb, 0, timer_interval/1000.0);
	ev_timer_start(loop, &clear_timer);

	ev_run(loop, 0);
	/*
	set_timer(epollfd,clear_timer_fd);

	//u32_t roller=0;

	*/
	myexit(0);
	return 0;
}
void print_help()
{
	char git_version_buf[100]={0};
	strncpy(git_version_buf,gitversion,10);

	printf("\n");
	printf("tinyPortMapper\n");
	printf("git version:%s    ",git_version_buf);
	printf("build date:%s %s\n",__DATE__,__TIME__);
	printf("repository: https://github.com/wangyu-/tinyPortMapper\n");
	printf("\n");
	printf("usage:\n");
	printf("    ./this_program  -l <listen_ip>:<listen_port> -r <remote_ip>:<remote_port>  [options]\n");
	printf("\n");

	printf("main options:\n");
	printf("    -t                                    enable TCP forwarding/mapping\n");
	printf("    -u                                    enable UDP forwarding/mapping\n");
	//printf("NOTE: If neither of -t or -u is provided,this program enables both TCP and UDP forward\n");
	printf("\n");

	printf("other options:\n");
	printf("    --sock-buf            <number>        buf size for socket, >=10 and <=10240, unit: kbyte, default: 1024\n");
	printf("    --log-level           <number>        0: never    1: fatal   2: error   3: warn \n");
	printf("                                          4: info (default)      5: debug   6: trace\n");
	printf("    --log-position                        enable file name, function name, line number in log\n");
	printf("    --disable-color                       disable log color\n");
	printf("    --enable-color                        enable log color, log color is enabled by default on most platforms\n");
	printf("    -h,--help                             print this help message\n");
	printf("\n");


	//printf("common options,these options must be same on both side\n");
}
void process_arg(int argc, char *argv[])
{
	int i, j, k;
	int opt;
    static struct option long_options[] =
      {
		{"log-level", required_argument,    0, 1},
		{"log-position", no_argument,    0, 1},
		{"disable-color", no_argument,    0, 1},
		{"enable-color", no_argument,    0, 1},
		{"sock-buf", required_argument,    0, 1},
		{NULL, 0, 0, 0}
      };
    int option_index = 0;
	if (argc == 1)
	{
		print_help();
		myexit( -1);
	}
	for (i = 0; i < argc; i++)
	{
		if(strcmp(argv[i],"-h")==0||strcmp(argv[i],"--help")==0)
		{
			print_help();
			myexit(0);
		}
	}
	for (i = 0; i < argc; i++)
	{
		if(strcmp(argv[i],"--log-level")==0)
		{
			if(i<argc -1)
			{
				sscanf(argv[i+1],"%d",&log_level);
				if(0<=log_level&&log_level<log_end)
				{
				}
				else
				{
					log_bare(log_fatal,"invalid log_level\n");
					myexit(-1);
				}
			}
		}
		if(strcmp(argv[i],"--disable-color")==0)
		{
			enable_log_color=0;
		}
		if(strcmp(argv[i],"--enable-color")==0)
		{
			enable_log_color=1;
		}
	}

    mylog(log_info,"argc=%d ", argc);

	for (i = 0; i < argc; i++) {
		log_bare(log_info, "%s ", argv[i]);
	}
	log_bare(log_info, "\n");

	if (argc == 1)
	{
		print_help();
		myexit(-1);
	}

	int no_l = 1, no_r = 1;
	while ((opt = getopt_long(argc, argv, "l:r:tuh:",long_options,&option_index)) != -1)
	{
		//string opt_key;
		//opt_key+=opt;
		switch (opt)
		{

		case 'l':
			no_l = 0;
			local_addr.from_str(optarg);
			break;
		case 'r':
			no_r = 0;
			remote_addr.from_str(optarg);
			break;
		case 't':
			enable_tcp=1;
			break;
		case 'u':
			enable_udp=1;
			break;
		case 'h':
			break;
		case 1:
			if(strcmp(long_options[option_index].name,"log-level")==0)
			{
			}
			else if(strcmp(long_options[option_index].name,"disable-color")==0)
			{
				//enable_log_color=0;
			}
			else if(strcmp(long_options[option_index].name,"enable-color")==0)
			{
				//enable_log_color=0;
			}
			else if(strcmp(long_options[option_index].name,"log-position")==0)
			{
				enable_log_position=1;
			}
			else if(strcmp(long_options[option_index].name,"sock-buf")==0)
			{
				int tmp=-1;
				sscanf(optarg,"%d",&tmp);
				if(10<=tmp&&tmp<=10*1024)
				{
					socket_buf_size=tmp*1024;
				}
				else
				{
					mylog(log_fatal,"sock-buf value must be between 1 and 10240 (kbyte) \n");
					myexit(-1);
				}
			}
			else
			{
				mylog(log_fatal,"unknown option\n");
				myexit(-1);
			}
			break;
		default:
			mylog(log_fatal,"unknown option <%x>", opt);
			myexit(-1);
		}
	}

	if (no_l)
		mylog(log_fatal,"error: -l not found\n");
	if (no_r)
		mylog(log_fatal,"error: -r not found\n");
	if (no_l || no_r)
		myexit(-1);

	if(enable_tcp==0&&enable_udp==0)
	{
		//enable_tcp=1;
		//enable_udp=1;
		mylog(log_fatal,"you must specify -t or -u or both\n");
		myexit(-1);
	}
}

int unit_test()
{
	//lru_cache_t<string,u64_t> cache;

	address_t::hash_function hash;
	address_t test;
	test.from_str((char*)"[2001:19f0:7001:1111:00:ff:11:22]:443");
	printf("%s\n",test.get_str());
	printf("%d\n",hash(test));
	test.from_str((char*)"44.55.66.77:443");
	printf("%s\n",test.get_str());
	printf("%d\n",hash(test));

	return 0;
}

int resolve_ipv6(char *domain, char *addr) {
    struct addrinfo hints, *res;
    int err;

#ifdef _WIN32
    WSADATA wsaData;
    // 初始化 Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return -1;
    }
#endif

    // 清空 hints 结构体并设置为查询 IPv6
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;  // 设置为 IPv6

    // 调用 getaddrinfo 进行解析
    err = getaddrinfo(domain, NULL, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(err));
#ifdef _WIN32
        WSACleanup();  // 清理 Winsock
#endif
        return -1;
    }

    // 将解析出的 IPv6 地址转换为字符串格式
    if (res != NULL) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)res->ai_addr;
        inet_ntop(AF_INET6, &ipv6->sin6_addr, addr, INET6_ADDRSTRLEN);
    }

    // 释放内存
    freeaddrinfo(res);

#ifdef _WIN32
    WSACleanup();  // 清理 Winsock
#endif

    return 0;
}

#ifdef _WIN32
// Windows 平台的子进程创建函数
void create_child_process(const char *program_name, const char *addr, u32_t port, HANDLE job_handle, HANDLE hPipeWrite) {
    // 创建命令行参数
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s -l 127.0.0.1:%d -r [%s]:%d -u", program_name, port, addr, port);

    // 转换为宽字符
    wchar_t wcmd[256];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 256);

    // 设置启动信息，重定向子进程的 stdout 到管道的写端
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    si.hStdOutput = hPipeWrite; // 将子进程的 stdout 重定向到管道
    si.dwFlags |= STARTF_USESTDHANDLES;

    // 启动子进程
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "Failed to create process for port %d. Error: %lu\n", port, GetLastError());
        exit(1);
    }

    // 将子进程添加到 Job Object 中
    if (!AssignProcessToJobObject(job_handle, pi.hProcess)) {
        fprintf(stderr, "Failed to assign process to job object. Error: %lu\n", GetLastError());
        exit(1);
    }

    // 确保子进程句柄关闭，避免资源泄露
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}
#else
// Unix/Linux 平台的子进程创建函数
void create_child_process(const char *program_name, const char *addr,int port) {
	// 未实现！！！
	// 相信会玩Linux的也不差这点命令执行功底吧。。
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed");
        exit(1);
    } else if (pid == 0) {
        // 子进程
        printf("Child process started for port %s\n", port);
		char * argv = malloc(sizeof(char)*1024);
		sprintf(argv,"%s -l 127.0.0.1:%d -r [%s]:%d -u",program_name,port,port,addr,port);
        execlp(program_name, program_name, port, NULL);
        // 如果 execlp 返回，说明失败
        perror("execlp failed");
        exit(1);
    } else {
        // 父进程保存子进程 PID，以便稍后终止
        printf("Child process PID: %d\n", pid);
    }
}
#endif

int main(int argc, char *argv[])
{
    init_ws();
    struct ev_loop* loop=ev_default_loop(0);
#if !defined(__MINGW32__)
    ev_signal signal_watcher_sigpipe;
    ev_signal_init(&signal_watcher_sigpipe, sigpipe_cb, SIGPIPE);
    ev_signal_start(loop, &signal_watcher_sigpipe);
#else
    enable_log_color=0;
    printf("supported_backends()=%x\n",ev_supported_backends());
    fflush(0);
#endif


    ev_signal signal_watcher_sigterm;
    ev_signal_init(&signal_watcher_sigterm, sigterm_cb, SIGTERM);
    ev_signal_start(loop, &signal_watcher_sigterm);

    ev_signal signal_watcher_sigint;
    ev_signal_init(&signal_watcher_sigint, sigint_cb, SIGINT);
    ev_signal_start(loop, &signal_watcher_sigint);

	//unit_test();
	assert(sizeof(u64_t)==8);
	assert(sizeof(i64_t)==8);
	assert(sizeof(u32_t)==4);
	assert(sizeof(i32_t)==4);
	dup2(1, 2);		//redirect stderr to stdout
	int i, j, k;
	
	// 定义默认参数，你可以在这里完成你的自定义，比如默认域名或端口
	const char * defaultDomain = "kore.host";
	u32_t port1 = 10999;
	u32_t port2 = 10998;
	// start --------------
	char *domain;
	// 中文输出 设置控制台编码UTF-8
	SetConsoleOutputCP(CP_UTF8);
	if(argc <3){ // 主进程逻辑
		puts("----------*****----------");
		puts("这是一个在原版(wangyu-/tinyPortMapper)基础上修改过的程序,目的是提供饥荒联机版的IPv6端口映射方式完成直连，以显著降低联机时延。\n");
		puts("你可以使用原版的参数列表，也可以使用快捷映射命令：./dstClientPortMapper.exe [domain] ");
		puts("如果你双击直接运行（不输入参数），则将使用域名 \"kore.host\" 作为缺省参数。");
		puts("该命令将会把域名的10999和10998端口映射到本地127.0.0.1地址上。进入饥荒联机版以后可以按'~'唤出控制台，输入 c_connect(\"127.0.0.1\") 然后回车即可\n");
		puts("直接关闭窗口即可结束映射。感谢你的使用。");
		puts("如果有侵权问题，请联系我。https://github.com/binbla\n");
		puts("如果你想要完成自己的修改，自己读源码吧。😋\n");
		puts("----------*****----------");
		if(argc == 1){
			domain = (char*)malloc(sizeof(char)*DOMAIN_MAX_LEN);
			strcpy(domain,defaultDomain);// 默认域名
			printf("use default domain: %s\n",defaultDomain);
		}else{
			domain = argv[1];
		}
		// 域名解析
		char addr[INET6_ADDRSTRLEN];
		if (resolve_ipv6(domain, addr) == 0) {
        	printf("IPv6 address for %s: %s\n", domain, addr);
    	} else {
			printf("Failed to resolve IPv6 address for %s\n", domain);
			exit(-1);
		}
		printf("Parent process started.\n");
#ifdef _WIN32 //windows 逻辑
		// 创建一个 Job Object 来管理所有子进程
		HANDLE job_handle = CreateJobObject(NULL, NULL);
		if (job_handle == NULL) {
			fprintf(stderr, "Failed to create job object. Error: %lu\n", GetLastError());
			exit(1);
		}
		// 创建管道，父进程从中读取数据
        HANDLE hPipeRead, hPipeWrite;
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
        if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) {
            fprintf(stderr, "CreatePipe failed\n");
            exit(1);
        }
		// 创建子进程并将它们添加到 Job Object 中
        create_child_process(argv[0], addr, port1, job_handle, hPipeWrite);
        create_child_process(argv[0], addr, port2, job_handle, hPipeWrite);
		// 父进程从管道读取并输出
        CloseHandle(hPipeWrite);  // 关闭管道的写端
        char buf[256];
        DWORD bytesRead;
        while (ReadFile(hPipeRead, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buf[bytesRead] = '\0';  // 添加 null 终止符
            printf("%s", buf);      // 打印子进程的输出
        }
		
		// 父进程等待子进程退出
		WaitForSingleObject(job_handle, INFINITE);
		// 清理 Job Object
		CloseHandle(job_handle);
#else	//unix 逻辑
		// 创建两个子进程
		create_child_process(argv[0], addr,port1);
		sleep(1);
		create_child_process(argv[0], addr,port2);
		// 父进程等待所有子进程退出（通过发送信号终止）
		// 父进程等待子进程（仅 Unix/Linux 需要，Windows 子进程生命周期与父进程绑定）
		while (wait(NULL) > 0); // 等待所有子进程退出
#endif
    	printf("Parent process exiting\n");
    	return 0;
	}else{// 子进程逻辑
		printf("Child process started\n");
        	process_arg(argc,argv);
		event_loop();
        	return 0;
	}
}
