#include "bittorrent.h"
#include "session_impl.h"

namespace libtorrent
{
	//////////////////////////////////////////////////////////////////////////
	//

	TorrentSession::TorrentSession( std::vector< std::string > const & sessionSettingParam, ErrorHandler error_handler, EventHandler event_handler, int listenPort )
		: impl_( new TorrentSessionImpl( listenPort, sessionSettingParam, error_handler, event_handler ) )
	{}

	//////////////////////////////////////////////////////////////////////////
	//

	TorrentSession::~TorrentSession() { if( impl_ ) delete impl_; };

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSession::update() { impl_->update(); }

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSession::add(std::string torrent) { return impl_->add( torrent ); }

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSession::del(std::string torrent, bool delete_torrent_file, bool delete_download_file) { 
		return impl_->del( torrent, delete_torrent_file, delete_download_file );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSession::pause( std::string torrent ) {
		return impl_->pause( torrent );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSession::resume( std::string torrent ) {
		return impl_->resume( torrent );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSession::setting( std::vector< std::string > const & params ) {
		return impl_->setting( params );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSession::set_print_debug( bool print ) {
		impl_->set_print_debug( print );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	std::string TorrentSession::get_tag( std::string torrent ) {
		return impl_->get_tag( torrent );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	std::string TorrentSession::get_magnet( std::string torrent ) {
		return impl_->get_magnet( torrent );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSession::get_metadata( std::string torrent, std::vector<char> & data ) {
		return impl_->get_metadata( torrent, data );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	void TorrentSession::set_sequential_down( std::string torrent, bool sequential ) {
		impl_->set_sequential_down( torrent, sequential );
	}

	//////////////////////////////////////////////////////////////////////////
	//

	bool TorrentSession::get_status( std::string torrent, TorrentStatus & ts )
	{
		return impl_->get_status( torrent, ts );
	}

	//////////////////////////////////////////////////////////////////////////
}