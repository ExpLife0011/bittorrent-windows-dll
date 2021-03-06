#include "session_impl.h"
#include "debug_print.hpp"

namespace libtorrent
{
	//////////////////////////////////////////////////////////////////////////
	//

	TorrentSessionImpl::TorrentSessionImpl( int listenPort, std::vector<std::string> const & sessionSettingParam, ErrorHandler error_handler, EventHandler event_handler ) 
		: listen_port_(listenPort)
		, allocation_mode_( libtorrent::storage_mode_sparse )
		, save_path_(".")
		, torrent_upload_limit_(0)
		, torrent_download_limit_(0)
		, bind_to_interface_("")
		, outgoing_interface_("")
		, poll_interval_(5)
		, max_connections_per_torrent_(50)
		, seed_mode_(false)
		, share_mode_(false)
		, disable_storage_(false)
		, start_dht_(true)
		, start_upnp_(true)
		, start_lsd_(true)
		, error_handler_(error_handler)
		, event_handler_(event_handler)
		//, active_torrent_(0)
		, next_dir_scan_(time_now())
		, num_outstanding_resume_data_(0)
		, print_debug_(false)
		, session_(fingerprint("LT", LIBTORRENT_VERSION_MAJOR, LIBTORRENT_VERSION_MINOR, 0, 0)
			, session::add_default_plugins
			, alert::all_categories
			& ~(alert::dht_notification
			+ alert::progress_notification
			+ alert::debug_notification
			+ alert::stats_notification))
	{
		load_setting();

		setting( sessionSettingParam, true );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSessionImpl::add( std::string const & torrent )
	{
		error_code ec;

		// match it against the <hash>@<tracker> format
		if (torrent.size() > 45
			&& is_hex(torrent.c_str(), 40)
			&& (strncmp(torrent.c_str() + 40, "@http://", 8) == 0
			|| strncmp(torrent.c_str() + 40, "@udp://", 7) == 0))
		{
			sha1_hash info_hash;
			from_hex(torrent.c_str(), 40, (char*)&info_hash[0]);

			add_torrent_params p;
			if (seed_mode_) p.flags |= add_torrent_params::flag_seed_mode;
			if (disable_storage_) p.storage = disabled_storage_constructor;
			if (share_mode_) p.flags |= add_torrent_params::flag_share_mode;
			p.trackers.push_back(torrent.c_str() + 41);
			p.info_hash = info_hash;
			p.save_path = save_path_;
			p.storage_mode = (storage_mode_t)allocation_mode_;
			p.flags |= add_torrent_params::flag_paused;
			p.flags &= ~add_torrent_params::flag_duplicate_is_error;
			p.flags |= add_torrent_params::flag_auto_managed;

			std::string tag;
			tag += "@tag:" + torrent;
			p.userdata = (void*)_strdup( tag.c_str() );

			session_.async_add_torrent( p );
		}
		else if (std::strstr(torrent.c_str(), "http://") == torrent.c_str()
			|| std::strstr(torrent.c_str(), "https://") == torrent.c_str()
			|| std::strstr(torrent.c_str(), "magnet:") == torrent.c_str())
		{
			add_torrent_params p;
			if (seed_mode_) p.flags |= add_torrent_params::flag_seed_mode;
			if (disable_storage_) p.storage = disabled_storage_constructor;
			if (share_mode_) p.flags |= add_torrent_params::flag_share_mode;
			p.save_path = save_path_;
			p.storage_mode = (storage_mode_t)allocation_mode_;
			p.url = torrent;

			std::vector<char> buf;
			if (std::strstr(torrent.c_str(), "magnet:") == torrent.c_str())
			{
				add_torrent_params tmp;
				parse_magnet_uri(torrent, tmp, ec);

				if (ec)
					return false;

				std::string filename = combine_path(save_path_, combine_path(".resume"
					, to_hex(tmp.info_hash.to_string()) + ".resume"));

				if (load_file(filename.c_str(), buf, ec) == 0)
					p.resume_data = &buf;
			}

			std::string tag;
			tag += "@tag:" + torrent;
			p.userdata = (void*)_strdup( tag.c_str() );

			//printf("adding URL: %s\n", torrent.c_str());
			session_.async_add_torrent(p);
		}
		else
		{
			// if it's a torrent file, open it as usual
			if( load_torrent( torrent ) )
			{
				return true;
			}
		}

		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSessionImpl::load_torrent( std::string const & torrent )
	{
		boost::intrusive_ptr<torrent_info> t;
		error_code ec;

		t = new torrent_info(torrent, ec);

		if (ec)
		{
			error_handler_( 0, (torrent + ": " + ec.message()).c_str() );
			t.reset();
			return false;
		}

		//printf("%s\n", t->name().c_str());

		add_torrent_params p;
		if (seed_mode_) p.flags |= add_torrent_params::flag_seed_mode;
		if (disable_storage_) p.storage = disabled_storage_constructor;
		if (share_mode_) p.flags |= add_torrent_params::flag_share_mode;
		lazy_entry resume_data;

		std::string filename = combine_path(save_path_, combine_path(".resume", to_hex(t->info_hash().to_string()) + ".resume"));

		std::vector<char> buf;
		if (load_file(filename.c_str(), buf, ec) == 0)
			p.resume_data = &buf;

		p.ti = t;
		p.save_path = save_path_;
		p.storage_mode = (storage_mode_t)allocation_mode_;
		p.flags |= add_torrent_params::flag_paused;
		p.flags &= ~add_torrent_params::flag_duplicate_is_error;
		p.flags |= add_torrent_params::flag_auto_managed;

		std::string tag;

		tag += "@isfile@tag:" + torrent;

		p.userdata = (void*)_strdup(tag.c_str());
		session_.async_add_torrent(p);

		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSessionImpl::del( std::string const & torrent, bool delete_torrent_file, bool delete_download_file )
	{
		std::string const & org_tag = torrent_tags_[torrent];

		auto entry = torrents_.find_iter<0>( org_tag );

		if( entry == torrents_.end<0>() )
			return false;

		torrent_handle const & handle = std::tr1::get<1>( *entry->second ).handle_;

		non_files_.erase(handle);
		
		
		auto file = files_.find_iter<0>( org_tag );

		if( file != files_.end<0>() )
		{
			// also delete the .torrent file from the torrent directory
			if( delete_torrent_file )
			{
				error_code ec;
				remove(combine_path("", std::tr1::get<1>( *file->second ).path_), ec);
			}

			files_.erase<0>(file);
		}


		if( handle.is_valid() )
		{
			session_.remove_torrent( handle, delete_download_file ? session::delete_files : session::none );
		}

		torrents_.erase<0>(entry);

		torrent_tags_.erase( torrent_tags_.find( org_tag ) );
		torrent_tags_.erase( torrent_tags_.find( torrent ) );

		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSessionImpl::pause( std::string const & torrent )
	{
		std::string const & org_tag = torrent_tags_[torrent];

		auto entry = torrents_.find<0>(org_tag);

		if( entry )
		{
			torrent_status const & status = std::tr1::get<1>( *entry ).status_;

			//if( !status.auto_managed && status.paused )
			//{
			//	status.handle.auto_managed( true );
			//}
			//else
			{
				status.handle.auto_managed( false );
				status.handle.pause(torrent_handle::graceful_pause);
			}

			return true;
		}

		return false;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSessionImpl::resume( std::string const & torrent )
	{
		std::string const & org_tag = torrent_tags_[torrent];

		auto entry = torrents_.find<0>( org_tag );

		if( entry )
		{
			torrent_status const & status = std::tr1::get<1>(*entry).status_;

			//status.handle.auto_managed(!status.auto_managed);
			status.handle.auto_managed( true );

			if( status.auto_managed && status.paused )
			{
				status.handle.resume();
			}

			return true;
		}

		return false;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSessionImpl::setting( std::vector<std::string> const & params, bool isFirst )
	{
		error_code ec;

		for (size_t i = 0; i < params.size(); ++i)
		{			
			if( params[i].empty() || params[i][0] != '-' )
				continue;

			// if there's a flag but no argument following, ignore it
			if (params.size() == (i+1))
				continue;

			char const* arg = params[i+1].c_str();

			switch (params[i][1])
			{
			case 'o': session_settings_.half_open_limit = atoi(arg); break;
			case 'h': session_settings_.allow_multiple_connections_per_ip = true; --i; break;
			case 'p': listen_port_ = atoi(arg); break;
			case 'k': session_settings_ = high_performance_seed(); --i; break;
			case 'j': session_settings_.use_disk_read_ahead = false; --i; break;
			case 'z': session_settings_.disable_hash_checks = true; --i; break;
			case 'B': session_settings_.peer_timeout = atoi(arg); break;
			case 'n': session_settings_.announce_to_all_tiers = true; --i; break;
			case 'G': seed_mode_ = true; --i; break;
			case 'd': session_settings_.download_rate_limit = atoi(arg) * 1000; break;
			case 'u': session_settings_.upload_rate_limit = atoi(arg) * 1000; break;
			case 'S': session_settings_.unchoke_slots_limit = atoi(arg); break;
			case 'a':
				if (strcmp(arg, "allocate") == 0) allocation_mode_ = storage_mode_allocate;
				if (strcmp(arg, "sparse") == 0) allocation_mode_ = storage_mode_sparse;
				break;
			case 's': save_path_ = arg; break;
			case 'U': torrent_upload_limit_ = atoi(arg) * 1000; break;
			case 'D': torrent_download_limit_ = atoi(arg) * 1000; break;
			case 'm': monitor_dir_ = arg; break;
			case 'Q': share_mode_ = true; --i; break;
			case 'b': bind_to_interface_ = arg; break;
			case 'w': session_settings_.urlseed_wait_retry = atoi(arg); break;
			case 't': poll_interval_ = atoi(arg); break;
			case 'H': start_dht_ = false; --i; break;
			case 'l': session_settings_.listen_queue_size = atoi(arg); break;
#ifndef TORRENT_DISABLE_ENCRYPTION
			case 'e':
				{
					pe_settings s;

					s.out_enc_policy = libtorrent::pe_settings::forced;
					s.in_enc_policy = libtorrent::pe_settings::forced;
					s.allowed_enc_level = pe_settings::rc4;
					s.prefer_rc4 = true;
					session_.set_pe_settings(s);
					break;
				}
#endif
			case 'W':
				session_settings_.max_peerlist_size = atoi(arg);
				session_settings_.max_paused_peerlist_size = atoi(arg) / 2;
				break;
			case 'x':
				{
					FILE* filter = fopen(arg, "r");
					if (filter)
					{
						ip_filter fil;
						unsigned int a,b,c,d,e,f,g,h, flags;
						while (fscanf(filter, "%u.%u.%u.%u - %u.%u.%u.%u %u\n", &a, &b, &c, &d, &e, &f, &g, &h, &flags) == 9)
						{
							address_v4 start((a << 24) + (b << 16) + (c << 8) + d);
							address_v4 last((e << 24) + (f << 16) + (g << 8) + h);
							if (flags <= 127) flags = ip_filter::blocked;
							else flags = 0;
							fil.add_rule(start, last, flags);
						}
						session_.set_ip_filter(fil);
						fclose(filter);
					}
				}
				break;
			case 'c': session_settings_.connections_limit = atoi(arg); break;
			case 'T': max_connections_per_torrent_ = atoi(arg); break;
#if TORRENT_USE_I2P
			case 'i':
				{
					proxy_settings ps;
					ps.hostname = arg;
					ps.port = 7656; // default SAM port
					ps.type = proxy_settings::i2p_proxy;
					session_.set_i2p_proxy(ps);
					break;
				}
#endif // TORRENT_USE_I2P
			case 'C':
				session_settings_.cache_size = atoi(arg);
				session_settings_.use_read_cache = session_settings_.cache_size > 0;
				session_settings_.cache_buffer_chunk_size = session_settings_.cache_size / 100;
				break;
			case 'A': session_settings_.allowed_fast_set_size = atoi(arg); break;
			case 'R': session_settings_.read_cache_line_size = atoi(arg); break;
			case 'O': session_settings_.allow_reordered_disk_operations = false; --i; break;
			case 'M': session_settings_.mixed_mode_algorithm = session_settings::prefer_tcp; --i; break;
			case 'y': session_settings_.enable_outgoing_tcp = false; session_settings_.enable_incoming_tcp = false; --i; break;
			case 'r': peer_ = arg; break;
			case 'P':
				{
					char* port = (char*) strrchr(arg, ':');
					if (port == 0)
					{
						fprintf(stderr, "invalid proxy hostname, no port found\n");
						break;
					}
					*port++ = 0;
					proxy_settings_.hostname = arg;
					proxy_settings_.port = atoi(port);
					if (proxy_settings_.port == 0) {
						fprintf(stderr, "invalid proxy port\n");
						break;
					}
					if (proxy_settings_.type == proxy_settings::none)
						proxy_settings_.type = proxy_settings::socks5;
				}
				break;
			case 'L':
				{
					char* pw = (char*) strchr(arg, ':');
					if (pw == 0)
					{
						fprintf(stderr, "invalid proxy username and password specified\n");
						break;
					}
					*pw++ = 0;
					proxy_settings_.username = arg;
					proxy_settings_.password = pw;
					proxy_settings_.type = proxy_settings::socks5_pw;
				}
				break;
			case 'I': outgoing_interface_ = arg; break;
			case 'N': start_upnp_ = false; --i; break;
			case 'X': start_lsd_ = false; --i; break;
			case 'Y': session_settings_.ignore_limits_on_local_network = false; --i; break;
			case 'v': session_settings_.active_downloads = atoi(arg);
				session_settings_.active_limit = (std::max)(atoi(arg) * 2, session_settings_.active_limit);
				break;
			case '^':
				session_settings_.active_seeds = atoi(arg);
				session_settings_.active_limit = (std::max)(atoi(arg) * 2, session_settings_.active_limit);
				break;
			case '0': disable_storage_ = true; --i;
			}
			++i; // skip the argument
		}

		// create directory for resume files
		create_directory(combine_path(save_path_, ".resume"), ec);

		if (ec)
		{
			error_handler_( 0, (std::string("failed to create resume file directory: ") + ec.message() ).c_str() );
			return false;
		}

		if (start_lsd_)
			session_.start_lsd();
		else
			session_.stop_lsd();

		if (start_upnp_)
		{
			session_.start_upnp();
			session_.start_natpmp();
		}
		else
		{
			session_.stop_upnp();
			session_.stop_natpmp();
		}

		session_.set_proxy(proxy_settings_);

		if( isFirst )
		{
			session_.listen_on( std::make_pair(listen_port_, listen_port_), ec, bind_to_interface_.c_str() );

			if (ec)
			{
				char msg[1024];

				sprintf_s( msg, sizeof(msg), "failed to listen on %s on ports %d-%d: %s\n"
					, bind_to_interface_.c_str(), listen_port_, listen_port_+1, ec.message().c_str() );

				error_handler_( 0, msg );

				return false;
			}
		}			

#ifndef TORRENT_DISABLE_DHT
		if (start_dht_)
		{
			session_settings_.use_dht_as_fallback = false;

			session_.add_dht_router(std::make_pair(
				std::string("router.bittorrent.com"), 6881));
			session_.add_dht_router(std::make_pair(
				std::string("router.utorrent.com"), 6881));
			session_.add_dht_router(std::make_pair(
				std::string("router.bitcomet.com"), 6881));

			session_.start_dht();
		}
#endif

		session_settings_.user_agent = "libtorrent/" LIBTORRENT_VERSION;
		session_settings_.choking_algorithm = session_settings::auto_expand_choker;
		session_settings_.disk_cache_algorithm = session_settings::avoid_readback;
		session_settings_.volatile_read_cache = false;

		session_.set_settings( session_settings_ );

		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSessionImpl::load_setting()
	{
		std::vector<char> in;
		error_code ec;
		if (load_file(".ses_state", in, ec) == 0)
		{
			lazy_entry e;
			if (lazy_bdecode(&in[0], &in[0] + in.size(), e, ec) == 0)
				session_.load_state(e);
		}

#ifndef TORRENT_DISABLE_GEO_IP
		session_.load_asnum_db("GeoIPASNum.dat");
		session_.load_country_db("GeoIP.dat");
#endif
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSessionImpl::update()
	{
		session_.post_torrent_updates();

		// loop through the alert queue to see if anything has happened.
		std::deque<alert*> alerts;
		session_.pop_alerts(&alerts);

		for (std::deque<alert*>::iterator i = alerts.begin(), end(alerts.end()); i != end; ++i)
		{
			bool need_resort = false;
			TORRENT_TRY
			{
				if (!handle_alert( *i ))
				{
					// if we didn't handle the alert, print it to the log
					std::string event_string;
					print_alert(*i, event_string);
					events_.push_back(event_string);
					if (events_.size() >= 20) events_.pop_front();
					//printf( "%s\n", event_string.c_str() );
				}
			} TORRENT_CATCH(std::exception& e) {}

			delete *i;
		}
		alerts.clear();
			

		print_debug();


		if (!monitor_dir_.empty()
			&& next_dir_scan_ < time_now())
		{
			scan_dir();

			next_dir_scan_ = time_now() + seconds(poll_interval_);
		}
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSessionImpl::print_debug()
	{
		if( !print_debug_ )
			return;

		std::string out;

		char str[1024];

		PeerInfos peer_infos;

		for( auto i = torrents_.begin<0>(); i != torrents_.end<0>(); ++i )
		{
			torrent_status const & status = std::tr1::get<1>( *i->second ).status_;

			if( !status.handle.is_valid() )
				continue;

			torrent_handle const & handle = status.handle;

			out += "====== ";
			out += handle.name();
			out += " ======\n";

			//if( status->state != torrent_status::seeding )
			handle.get_peer_info( peer_infos );

			if( peer_infos.empty() == false )
			{
				print_peer_info( out, peer_infos );
			}

			// tracker

			std::vector<announce_entry> tr = handle.trackers();
			ptime now = time_now();
			for (std::vector<announce_entry>::iterator i = tr.begin()
				, end(tr.end()); i != end; ++i)
			{
				snprintf(str, sizeof(str), "%2d %-55s fails: %-3d (%-3d) %s %s %5d \"%s\" %s\n"
					, i->tier, i->url.c_str(), i->fails, i->fail_limit, i->verified?"OK ":"-  "
					, i->updating?"updating"
					:!i->will_announce(now)?""
					:to_string(total_seconds(i->next_announce - now), 8).c_str()
					, i->min_announce > now ? total_seconds(i->min_announce - now) : 0
					, i->last_error ? i->last_error.message().c_str() : ""
					, i->message.c_str());
				out += str;
				out += "\n";
			}

			// download

			if( status.state == torrent_status::seeding )
			{
				out += "seeding\n";
			}
			else if( status.has_metadata )
			{
				std::vector<size_type> file_progress;
				handle.file_progress(file_progress);
				torrent_info const& info = handle.get_torrent_info();
				for (int i = 0; i < info.num_files(); ++i)
				{
					bool pad_file = info.file_at(i).pad_file;
					if (pad_file) continue;
					int progress = (int)(info.file_at(i).size > 0
						?file_progress[i] * 1000 / info.file_at(i).size:1000);

					char const* color = (file_progress[i] == info.file_at(i).size)
						?"32":"33";

					snprintf(str, sizeof(str), "%s %s %-5.2f%% %s %s%s\n",
						progress_bar(progress, 100, color).c_str()
						, pad_file?esc("34"):""
						, progress / 10.f
						, add_suffix((float)file_progress[i]).c_str()
						, convert_from_native( filename(info.files().file_path(info.file_at(i))) ).c_str()
						, pad_file?esc("0"):"");
					out += str;
				}
			}

			out += "___________________________________\n";
		}

		clear_home();
		puts(out.c_str());
		fflush(stdout);
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSessionImpl::scan_dir()
	{
		std::set<std::string> valid;

		using namespace libtorrent;

		if( monitor_dir_.empty() )
			return;

		error_code ec;

		for (directory i(monitor_dir_, ec); !i.done(); i.next(ec))
		{
			std::string file = combine_path(monitor_dir_, i.file());
			if (extension(file) != ".torrent") continue;

			if ( files_.find<0>(file) )
			{
				valid.insert(file);
				continue;
			}

			// the file has been added to the dir, start
			// downloading it.
			add(file);
			valid.insert(file);
		}

		// remove the torrents that are no longer in the directory
		for( auto i=valid.begin(); i!=valid.end(); ++i )
		{
			auto file = files_.find_iter<0>(*i);

			if( file != files_.end<0>() )
			{
				auto & handle = std::tr1::get<1>( *file->second ).handle_;

				if( !handle.is_valid() )
				{
					files_.erase<0>( file );
					continue;
				}

				handle.auto_managed(false);
				handle.pause();

				// the alert handler for save_resume_data_alert
				// will save it to disk
				if (handle.need_save_resume_data())
				{
					handle.save_resume_data();
					++num_outstanding_resume_data_;
				}

				files_.erase<0>(file);
			}
		}
		
		/*
		for (FileHandles::iterator i = files_.begin<0>(); !files_.empty() && i != files_.end<0>();)
		{
			if (i->first.empty() || valid.find(i->first) != valid.end())
			{
				++i;
				continue;
			}

			torrent_handle& h = i->second;
			if (!h.is_valid())
			{
				files_.erase(i++);
				continue;
			}

			h.auto_managed(false);
			h.pause();
			// the alert handler for save_resume_data_alert
			// will save it to disk
			if (h.need_save_resume_data())
			{
				h.save_resume_data();
				++num_outstanding_resume_data_;
			}

			files_.erase(i++);
		}
		*/
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSessionImpl::save_setting()
	{
		// keep track of the number of resume data
		// alerts to wait for
		int num_paused = 0;
		int num_failed = 0;

		session_.pause();

		// saving resume data
		std::vector<torrent_status> temp;
		session_.get_torrent_status(&temp, &yes, 0);

		for (std::vector<torrent_status>::iterator i = temp.begin(); i != temp.end(); ++i)
		{
			torrent_status& st = *i;
			if (!st.handle.is_valid())
			{
				//printf("  skipping, invalid handle\n");
				continue;
			}
			if (!st.has_metadata)
			{
				//printf("  skipping %s, no metadata\n", st.handle.name().c_str());
				continue;
			}
			if (!st.need_save_resume)
			{
				//printf("  skipping %s, resume file up-to-date\n", st.handle.name().c_str());
				continue;
			}

			// save_resume_data will generate an alert when it's done
			st.handle.save_resume_data();
			++num_outstanding_resume_data_;
			//printf("\r%d  ", num_outstanding_resume_data);
		}
		//printf("\nwaiting for resume data [%d]\n", num_outstanding_resume_data);


		while (num_outstanding_resume_data_ > 0)
		{
			alert const* a = session_.wait_for_alert(seconds(10));
			if (a == 0) continue;

			std::deque<alert*> alerts;
			session_.pop_alerts(&alerts);
			std::string now = time_now_string();
			for (std::deque<alert*>::iterator i = alerts.begin()
				, end(alerts.end()); i != end; ++i)
			{
				// make sure to delete each alert
				std::auto_ptr<alert> a(*i);

				torrent_paused_alert const* tp = alert_cast<torrent_paused_alert>(*i);
				if (tp)
				{
					++num_paused;
					//printf("\rleft: %d failed: %d pause: %d "
					//, num_outstanding_resume_data_, num_failed, num_paused);
					continue;
				}

				if (alert_cast<save_resume_data_failed_alert>(*i))
				{
					++num_failed;
					--num_outstanding_resume_data_;
					//printf("\rleft: %d failed: %d pause: %d "
					//, num_outstanding_resume_data_, num_failed, num_paused);
					continue;
				}

				save_resume_data_alert const* rd = alert_cast<save_resume_data_alert>(*i);
				if (!rd) continue;
				--num_outstanding_resume_data_;
				//printf("\rleft: %d failed: %d pause: %d "
				//, num_outstanding_resume_data_, num_failed, num_paused);

				if (!rd->resume_data) continue;

				torrent_handle h = rd->handle;
				std::vector<char> out;
				bencode(std::back_inserter(out), *rd->resume_data);
				save_file(combine_path(h.save_path(), combine_path(".resume", to_hex(h.info_hash().to_string()) + ".resume")), out);
			}
		}

		entry session_state;
		session_.save_state(session_state);

		std::vector<char> out;
		bencode(std::back_inserter(out), session_state);
		save_file(".ses_state", out);
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSessionImpl::handle_alert( libtorrent::alert* a )
	{
		using namespace libtorrent;

#ifdef TORRENT_USE_OPENSSL
		if (torrent_need_cert_alert* p = alert_cast<torrent_need_cert_alert>(a))
		{
			torrent_handle h = p->handle;
			error_code ec;
			file_status st;
			std::string cert = combine_path("certificates", to_hex(h.info_hash().to_string())) + ".pem";
			std::string priv = combine_path("certificates", to_hex(h.info_hash().to_string())) + "_key.pem";
			stat_file(cert, &st, ec);
			if (ec)
			{
				//char msg[256];
				//snprintf(msg, sizeof(msg), "ERROR. could not load certificate %s: %s\n", cert.c_str(), ec.message().c_str());
				//if (g_log_file) fprintf(g_log_file, "[%s] %s\n", time_now_string(), msg);
				return true;
			}
			stat_file(priv, &st, ec);
			if (ec)
			{
				//char msg[256];
				//snprintf(msg, sizeof(msg), "ERROR. could not load private key %s: %s\n", priv.c_str(), ec.message().c_str());
				//if (g_log_file) fprintf(g_log_file, "[%s] %s\n", time_now_string(), msg);
				return true;
			}

			//char msg[256];
			//snprintf(msg, sizeof(msg), "loaded certificate %s and key %s\n", cert.c_str(), priv.c_str());
			//if (g_log_file) fprintf(g_log_file, "[%s] %s\n", time_now_string(), msg);

			h.set_ssl_certificate(cert, priv, "certificates/dhparams.pem", "1234");
			h.resume();
		}
#endif

		if (metadata_received_alert* p = alert_cast<metadata_received_alert>(a))
		{
			// if we have a monitor dir, save the .torrent file we just received in it
			// also, add it to the files map, and remove it from the non_files list
			// to keep the scan dir logic in sync so it's not removed, or added twice
			torrent_handle h = p->handle;

			if (h.is_valid()) {
				torrent_info const& ti = h.get_torrent_info();
				create_torrent ct(ti);
				entry te = ct.generate();
				std::vector<char> buffer;
				bencode(std::back_inserter(buffer), te);
				std::string filename = ti.name() + "." + to_hex(ti.info_hash().to_string()) + ".torrent";
				filename = combine_path(monitor_dir_, filename);
				save_file(filename, buffer);

				auto entry = torrents_.find<1>(h);

				if( entry )
				{
					std::string const & tag = std::tr1::get<0>( *entry );
					files_.insert(tag, TorrentFile( filename, h ) );
					non_files_.erase(h);
				}			
			}
		}
		else if (add_torrent_alert* p = alert_cast<add_torrent_alert>(a))
		{
			std::string tag;
			if (p->params.userdata)
			{
				tag = (char*)p->params.userdata;
				free(p->params.userdata);
			}

			if (p->error)
			{
				//fprintf(stderr, "failed to add torrent: %s %s\n", filename.c_str(), p->error.message().c_str());
				char msg[1024];
				sprintf_s( msg, sizeof(msg), "failed to add torrent: %s %s\n", tag.c_str(), p->error.message().c_str());
				event_handler_( torrent_event_add_failed, msg );
			}
			else
			{
				torrent_handle h = p->handle;

				std::string filename;

				bool isFile = tag.find( "@isfile" ) != std::string::npos;

				tag = tag.substr( tag.find("@tag:")+5, std::string::npos );


				auto insert_faile = torrents_.insert(tag, TorrentEntry(h) );

				auto entry = torrents_.find< 1 >(TorrentEntry(h));

				torrent_tags_[ tag ] = std::tr1::get<0>( *entry );

				if( !insert_faile )
				{
					if (isFile)
					{
						files_.insert( tag, TorrentFile( tag, h ) );
					}
					else
					{
						non_files_.insert(h);
					}

					h.set_max_connections(max_connections_per_torrent_);
					h.set_max_uploads(-1);
					h.set_upload_limit(torrent_upload_limit_);
					h.set_download_limit(torrent_download_limit_);
					h.use_interface(outgoing_interface_.c_str());
#ifndef TORRENT_DISABLE_RESOLVE_COUNTRIES
					h.resolve_countries(true);
#endif

					// if we have a peer specified, connect to it
					if (!peer_.empty())
					{
						char* port = (char*) strrchr((char*)peer_.c_str(), ':');
						if (port > 0)
						{
							*port++ = 0;
							char const* ip = peer_.c_str();
							int peer_port = atoi(port);
							error_code ec;
							if (peer_port > 0)
								h.connect_peer(tcp::endpoint(address::from_string(ip, ec), peer_port));
						}
					}
				}
			}
		}
		else if (torrent_finished_alert* p = alert_cast<torrent_finished_alert>(a))
		{
			p->handle.set_max_connections(max_connections_per_torrent_ / 2);

			// write resume data for the finished torrent
			// the alert handler for save_resume_data_alert
			// will save it to disk
			torrent_handle h = p->handle;
			h.save_resume_data();
			++num_outstanding_resume_data_;

			event_handler_( torrent_event_finished, h.name() );
		}
		else if (save_resume_data_alert* p = alert_cast<save_resume_data_alert>(a))
		{
			--num_outstanding_resume_data_;
			torrent_handle h = p->handle;
			TORRENT_ASSERT(p->resume_data);
			if (p->resume_data)
			{
				std::vector<char> out;
				bencode(std::back_inserter(out), *p->resume_data);
				save_file(combine_path(h.save_path(), combine_path(".resume", to_hex(h.info_hash().to_string()) + ".resume")), out);
				if (h.is_valid()
					&& non_files_.find(h) == non_files_.end()
					&& !files_.find<1>(TorrentFile("",h)) )
					session_.remove_torrent(h);
			}
		}
		else if (save_resume_data_failed_alert* p = alert_cast<save_resume_data_failed_alert>(a))
		{
			--num_outstanding_resume_data_;
			torrent_handle h = p->handle;
			if (h.is_valid()
				&& non_files_.find(h) == non_files_.end()
				&& !files_.find<1>(TorrentFile("",h)) )
				session_.remove_torrent(h);
		}
		else if (torrent_paused_alert* p = alert_cast<torrent_paused_alert>(a))
		{
			// write resume data for the finished torrent
			// the alert handler for save_resume_data_alert
			// will save it to disk
			torrent_handle h = p->handle;
			h.save_resume_data();
			++num_outstanding_resume_data_;
		}
		else if (state_update_alert* p = alert_cast<state_update_alert>(a))
		{
			for (auto i = p->status.begin(); i != p->status.end(); ++i)
			{
				auto entry = torrents_.find<1>(i->handle);

				// don't add new entries here, that's done in the handler
				// for add_torrent_alert
				if ( !entry ) continue;

				torrent_status & status = const_cast< torrent_status& >( std::tr1::get<1>( *entry ).status_ );

				if (status.state != i->state
					|| status.paused != i->paused
					|| status.auto_managed != i->auto_managed)
				{
					status = *i;
				}
			}

			return true;
		}
		return false;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	TorrentSessionImpl::~TorrentSessionImpl()
	{
		save_setting();
	}

	//////////////////////////////////////////////////////////////////////////
	//

	std::string const & TorrentSessionImpl::get_tag( std::string const & torrent )
	{
		return torrent_tags_[torrent];
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSessionImpl::set_print_debug( bool print )
	{
		print_debug_ = print;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	std::string TorrentSessionImpl::get_magnet( std::string const & torrent )
	{
		std::string magnet;

		auto entry = torrents_.find<0>( torrent );

		if( entry )
		{
			torrent_handle const & handle = std::tr1::get<1>(*entry).handle_;

			if( handle.is_valid() )
				magnet = make_magnet_uri( std::tr1::get<1>(*entry).handle_ );
		}

		return magnet;
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSessionImpl::get_metadata( std::string const & torrent, std::vector<char> & data )
	{
		auto entry = torrents_.find<0>(torrent);

		if( entry )
		{
			torrent_handle const & handle = std::tr1::get<1>(*entry).handle_;

			if( handle.is_valid() )
			{
				torrent_info const & info = std::tr1::get<1>(*entry).handle_.get_torrent_info();

				if( data.capacity() < info.m_info_section_size )
					data.reserve( info.m_info_section_size );

				data.resize( info.m_info_section_size );

				memcpy_s( &data[0], data.size(), info.m_info_section.get(), info.m_info_section_size );
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSessionImpl::set_sequential_down( std::string const & torrent, bool sequential )
	{
		auto entry = torrents_.find<0>(torrent);

		if( entry )
		{
			torrent_handle const & handle = std::tr1::get<1>(*entry).handle_;

			if( handle.is_valid() )
			{
				handle.set_sequential_download( sequential );
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSessionImpl::get_status( std::string const & torrent, TorrentStatus & ts )
	{
		auto entry = torrents_.find<0>(torrent);

		if( entry )
		{
			torrent_status const & status = std::tr1::get<1>(*entry).status_;

#define _copy_to_ts( var ) ts.##var = status.##var;

			ts.state = (TorrentStatus::state_t)status.state;
			_copy_to_ts( paused );
			_copy_to_ts( auto_managed );
			_copy_to_ts( sequential_download );
			_copy_to_ts( is_seeding );
			_copy_to_ts( is_finished );
			_copy_to_ts( has_metadata );
			_copy_to_ts( progress );
			_copy_to_ts( progress_ppm );
			_copy_to_ts( current_tracker );
			_copy_to_ts( total_download );
			_copy_to_ts( total_upload );
			_copy_to_ts( total_payload_download );
			_copy_to_ts( total_payload_upload );
			_copy_to_ts( total_failed_bytes );
			_copy_to_ts( total_redundant_bytes );
			_copy_to_ts( download_rate );
			_copy_to_ts( upload_rate );
			_copy_to_ts( download_payload_rate );
			_copy_to_ts( upload_payload_rate );
			_copy_to_ts( num_seeds );
			_copy_to_ts( num_peers );
			_copy_to_ts( num_complete );
			_copy_to_ts( num_incomplete );
			_copy_to_ts( list_seeds );
			_copy_to_ts( list_peers );
			_copy_to_ts( connect_candidates );
			_copy_to_ts( num_pieces );
			_copy_to_ts( total_done );
			_copy_to_ts( total_wanted_done );
			_copy_to_ts( total_wanted );
			_copy_to_ts( distributed_full_copies );
			_copy_to_ts( distributed_fraction );
			_copy_to_ts( distributed_copies );
			_copy_to_ts( block_size );
			_copy_to_ts( num_uploads );
			_copy_to_ts( num_connections );
			_copy_to_ts( uploads_limit );
			_copy_to_ts( connections_limit );
			_copy_to_ts( up_bandwidth_queue );
			_copy_to_ts( down_bandwidth_queue );
			_copy_to_ts( all_time_upload );
			_copy_to_ts( all_time_download );
			_copy_to_ts( active_time );
			_copy_to_ts( finished_time );
			_copy_to_ts( seeding_time );
			_copy_to_ts( seed_rank );
			_copy_to_ts( last_scrape );
			_copy_to_ts( has_incoming );
			_copy_to_ts( sparse_regions );
			_copy_to_ts( seed_mode );
			_copy_to_ts( upload_mode );
			_copy_to_ts( share_mode );
			_copy_to_ts( super_seeding );
			_copy_to_ts( priority );
			_copy_to_ts( added_time );
			_copy_to_ts( completed_time );
			_copy_to_ts( last_seen_complete );
			_copy_to_ts( time_since_upload );
			_copy_to_ts( time_since_download );
			_copy_to_ts( queue_position );
			_copy_to_ts( need_save_resume );
			_copy_to_ts( ip_filter_applies );
			_copy_to_ts( listen_port );

#undef _copy_to_ts

			return true;
		}

		return false;
	}

	//////////////////////////////////////////////////////////////////////////
}