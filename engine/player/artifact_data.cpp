#include "simulationcraft.hpp"

#include "artifact_data.hpp"

namespace
{
template <class ...Args>
void debug( const artifact::player_artifact_data_t* obj, Args&& ...args )
{
  if ( obj -> sim() -> debug )
  {
    obj -> sim() -> out_debug.printf( std::forward<Args>( args )... );
  }
}

template <class ...Args>
void error( const artifact::player_artifact_data_t* obj, Args&& ...args )
{
  obj -> sim() -> errorf( std::forward<Args>( args )... );
}
}

namespace artifact
{
artifact_data_ptr_t player_artifact_data_t::create( player_t* player )
{
  artifact_data_ptr_t obj { new player_artifact_data_t( player ) };

  return obj;
}

artifact_data_ptr_t player_artifact_data_t::create( const artifact_data_ptr_t& other )
{
  artifact_data_ptr_t obj { new player_artifact_data_t( *other.get() ) };

  return obj;
}

player_artifact_data_t::player_artifact_data_t( player_t* player ) :
  m_player( player ), m_has_relic_opts( false ), m_total_points( 0 ),
  m_purchased_points( 0 ), m_crucible_points( 0 ), m_slot( SLOT_INVALID ),
  m_artificial_stamina( spell_data_t::not_found() ),
  m_artificial_damage( spell_data_t::not_found() )
{ }

const point_data_t& player_artifact_data_t::point_data( unsigned power_id ) const
{
  static const point_data_t __default = point_data_t();

  auto it = m_points.find( power_id );

  return it != m_points.end() ? it -> second : __default;
}

bool player_artifact_data_t::initialize()
{
  if ( sim() -> disable_artifacts )
  {
    return true;
  }

  auto p = powers();

  auto damage_it = range::find_if( p , [ this ]( const artifact_power_data_t* power ) {
    auto spell_data = player() -> find_spell( power -> power_spell_id );
    return util::str_compare_ci( spell_data -> name_cstr(), "Artificial Damage" );
  } );

  m_artificial_damage = damage_it != p.end()
                        ? player() -> find_spell( ( *damage_it ) -> power_spell_id )
                        : spell_data_t::not_found();

  auto stamina_it = range::find_if( p, [ this ]( const artifact_power_data_t* power ) {
    auto spell_data = player() -> find_spell( power -> power_spell_id );
    return util::str_compare_ci( spell_data -> name_cstr(), "Artificial Stamina" );
  } );

  m_artificial_stamina = stamina_it != p.end()
                         ? player() -> find_spell( ( *stamina_it ) -> power_spell_id )
                         : spell_data_t::not_found();

  // Set the primary artifact slot for this player. It will be the item that has an artifact
  // identifier on it and it does not have a parent slot as per client data. The parent slot
  // initialization is performed in player_t::init_items.
  range::for_each( player() -> items, [ this ]( const item_t& i ) {
    if ( i.parsed.data.id_artifact > 0 && i.parent_slot == SLOT_INVALID )
    {
      assert( m_slot == SLOT_INVALID );
      debug( this, "Player %s setting artifact slot to '%s'", player() -> name(),
        util::slot_type_string( i.slot ) );
      m_slot = i.slot;
    }
  } );

  return parse() && parse_crucible();
}

bool player_artifact_data_t::enabled() const
{
  if ( sim() -> disable_artifacts )
  {
    return false;
  }
  else
  {
    return m_slot != SLOT_INVALID;
  }
}

sim_t* player_artifact_data_t::sim() const
{ return m_player -> sim; }

player_t* player_artifact_data_t::player() const
{ return m_player; }

void player_artifact_data_t::set_artifact_str( const std::string& value )
{
  debug( this, "Player %s setting artifact input to '%s'", player() -> name(), value.c_str() );
  m_artifact_str = value;
}

void player_artifact_data_t::set_crucible_str( const std::string& value )
{
  debug( this, "Player %s setting artifact crucible input to '%s'", player() -> name(), value.c_str() );
  m_crucible_str = value;
}

bool player_artifact_data_t::add_power( unsigned power_id, unsigned rank )
{
  if ( power_id == 0 || rank == 0 || rank > MAX_TRAIT_RANK )
  {
    debug( this, "Player %s invalid power, id=%u rank=%u", player() -> name(), power_id, rank );
    return false;
  }

  // Note, must not use overrides here to ensure artifact input parsing is not affected
  const auto& pd = point_data( power_id );
  if ( pd.purchased > 0 )
  {
    debug( this, "Player%s re-adding power %u", player() -> name(), power_id );
    return false;
  }

  auto it = m_points.find( power_id );
  if ( it != m_points.end() )
  {
    m_points[ power_id ].purchased += as<uint8_t>( rank );
  }
  else
  {
    m_points[ power_id ] = { as<uint8_t>( rank ), 0U };
  }

  m_total_points += rank;
  m_purchased_points += rank;

  if ( sim() -> debug )
  {
    debug( this, "Player %s added artifact trait '%s' (id=%u), rank=%u",
      player() -> name(), power( power_id ) -> name, power_id, rank );
  }

  return true;
}

bool player_artifact_data_t::add_crucible_power( unsigned power_id, unsigned rank )
{
  if ( power_id == 0 || rank == 0 || rank > MAX_TRAIT_RANK )
  {
    debug( this, "Player %s invalid crucible power, id=%u rank=%u", player() -> name(), power_id, rank );
    return false;
  }

  if ( ! valid_crucible_power( power_id ) )
  {
    debug( this, "Player %s invalid crucible power, id=%u", player() -> name(), power_id );
    return false;
  }

  const auto& pd = point_data( power_id );
  if ( pd.crucible > 0 )
  {
    debug( this, "Player %s re-adding crucible power %u", player() -> name(), power_id );
    return false;
  }

  m_points[ power_id ].crucible = as<uint8_t>( rank );

  m_total_points += rank;
  m_crucible_points += rank;

  if ( sim() -> debug )
  {
    debug( this, "Player %s added crucible artifact trait '%s' (id=%u), rank=%u",
      player() -> name(), power( power_id ) -> name, power_id, rank );
  }

  return true;
}

void player_artifact_data_t::override_power( const std::string& name_str, unsigned rank )
{
  if ( rank > MAX_TRAIT_RANK || name_str.empty() || ! enabled() )
  {
    return;
  }

  auto p = powers();
  auto it = range::find_if( p, [ &name_str ]( const artifact_power_data_t* power ) {
    std::string power_name = power -> name;
    util::tokenize( power_name );
    return util::str_compare_ci( name_str, power_name );
  } );

  if ( it == p.end() )
  {
    error( this, "Player %s unknown artifact power name '%s' to override",
      player() -> name(), name_str.c_str() );
    return;
  }

  if ( rank > ( *it ) -> max_rank )
  {
    error( this, "Player %s too high rank '%u' for '%s', expected %u max",
      player() -> name(), rank, ( *it ) -> name, ( *it ) -> max_rank );
    return;
  }

  // Remove old ranks
  const auto& pd = point_data( ( *it ) -> id );
  if ( pd.overridden > 0 )
  {
    m_total_points -= pd.overridden;
    m_purchased_points -= pd.overridden;
  }
  else
  {
    m_total_points -= pd.purchased + pd.crucible + pd.bonus;
    m_purchased_points -= pd.purchased;
    m_crucible_points -= pd.crucible;
  }

  // Inject the overridden rank as purely purchased points
  m_total_points += rank;
  m_purchased_points += rank;

  auto point_it = m_points.find( ( *it ) -> id );
  if ( point_it == m_points.end() )
  {
    m_points[ ( *it ) -> id ] = { 0U, 0U, as<int16_t>( rank ) };
  }
  else
  {
    m_points[ ( *it ) -> id ].overridden = as<int16_t>( rank );
  }
}

void player_artifact_data_t::add_relic( unsigned item_id,
                                        unsigned power_id,
                                        unsigned rank )
{
  if ( item_id == 0 || power_id == 0 || rank == 0 || rank > MAX_TRAIT_RANK )
  {
    debug( this, "Player %s invalid relic id=%u, power=%u, rank=%u",
      player() -> name(), item_id, power_id, rank );
    return;
  }

  auto it = m_points.find( power_id );
  if ( it == m_points.end() )
  {
    m_points[ power_id ] = { 0U, as<uint8_t>( rank ) };
  }
  else if ( m_points[ power_id ].bonus + rank <= MAX_TRAIT_RANK )
  {
    m_points[ power_id ].bonus += rank;
  }

  m_total_points += rank;

  if ( sim() -> debug )
  {
    debug( this, "Player %s added artifact relic trait '%s' (id=%u, item_id=%u), rank=%u",
      player() -> name(), power( power_id ) -> name, power_id, item_id, rank );
  }
}

void player_artifact_data_t::move_purchased_rank( unsigned power_id, unsigned rank )
{
  if ( power_id == 0 || rank == 0 || rank > MAX_TRAIT_RANK )
  {
    debug( this, "Player %s invalid power to move, id=%u rank=%u", player() -> name(), power_id, rank );
    return;
  }

  auto it = m_points.find( power_id );
  if ( it == m_points.end() )
  {
    debug( this, "Player %s no trait for power id=%u", player() -> name(), power_id );
    return;
  }

  // Ensure we don't underflow
  auto max_removed = std::min( it -> second.purchased, as<uint8_t>( rank ) );

  it -> second.purchased -= max_removed;
  it -> second.bonus += max_removed;
  m_purchased_points -= max_removed;

  // Moving around purchased ranks indicates there are relics in the artifact trait data
  // automatically
  m_has_relic_opts = true;

  if ( sim() -> debug )
  {
    debug( this, "Player %s moved rank for artifact trait '%s' (id=%u), rank=%u",
      player() -> name(), power( power_id ) -> name, power_id, rank );
  }
}

std::string player_artifact_data_t::encode() const
{
  if ( ! m_artifact_str.empty() )
  {
    return m_artifact_str;
  }

  std::stringstream s;

  auto artifact_id = m_player -> dbc.artifact_by_spec( m_player -> specialization() );
  if ( artifact_id == 0 )
  {
    return std::string();
  }

  // Artifact id, then 4 zeros (relics), simc uses the weapon relic data for this
  s << artifact_id << ":0:0:0:0";

  // Order artifact powers statically
  auto order = powers();
  range::for_each( order, [ &s, this ]( const artifact_power_data_t* power ) {
    auto it = m_points.find( power -> id );
    if ( it != m_points.end() && it -> second.purchased > 0 )
    {
      s << ":" << it -> first << ":" << +it -> second.purchased;
    }
  } );

  return s.str();
}

std::string player_artifact_data_t::encode_crucible() const
{
  if ( ! m_crucible_str.empty() )
  {
    return m_crucible_str;
  }

  std::stringstream s;

  auto artifact_id = m_player -> dbc.artifact_by_spec( m_player -> specialization() );
  if ( artifact_id == 0 )
  {
    return std::string();
  }

  // Order crucible artifact powers statically
  auto order = powers();
  range::for_each( order, [ &s, this ]( const artifact_power_data_t* power ) {
    auto it = m_points.find( power -> id );
    if ( it != m_points.end() && it -> second.crucible > 0 )
    {
      s << ":" << it -> first << ":" << +it -> second.crucible;
    }
  } );

  return s.str();
}

std::string player_artifact_data_t::artifact_option_string() const
{
  if ( ! enabled() || m_player -> specialization() == SPEC_NONE )
  {
    return std::string();
  }

  auto artifact_id = m_player -> dbc.artifact_by_spec( m_player -> specialization() );
  if ( artifact_id == 0 )
  {
    return std::string();
  }

  auto encoded_data = encode();
  if ( ! encoded_data.empty() )
  {
    return "artifact=" + encoded_data;
  }
  else
  {
    return encoded_data;
  }
}

std::string player_artifact_data_t::crucible_option_string() const
{
  if ( ! enabled() || m_player -> specialization() == SPEC_NONE )
  {
    return std::string();
  }

  auto artifact_id = m_player -> dbc.artifact_by_spec( m_player -> specialization() );
  if ( artifact_id == 0 )
  {
    return std::string();
  }

  auto encoded_data = encode_crucible();
  if ( ! encoded_data.empty() )
  {
    return "crucible=" + encoded_data;
  }
  else
  {
    return encoded_data;
  }
}

std::vector<const artifact_power_data_t*> player_artifact_data_t::powers() const
{
  auto artifact_id = player() -> dbc.artifact_by_spec( player() -> specialization() );
  if ( artifact_id == 0 )
  {
    return { };
  }

  return player() -> dbc.artifact_powers( artifact_id );
}

const artifact_power_data_t* player_artifact_data_t::power( unsigned power_id ) const
{ return player() -> dbc.artifact_power( power_id ); }

bool player_artifact_data_t::valid_power( unsigned power_id ) const
{
  // Ensure artifact power belongs to the specialization
  auto p = powers();
  auto it = range::find_if( p, [ power_id ]( const artifact_power_data_t* power ) {
      return power_id == power -> id;
  } );

  return it != p.end();
}

bool player_artifact_data_t::valid_crucible_power( unsigned power_id ) const
{
  // Ensure artifact power belongs to the specialization
  auto p = powers();
  auto it = range::find_if( p, [ power_id ]( const artifact_power_data_t* power ) {
      return power_id == power -> id;
  } );

  if ( it == p.end() )
  {
    return false;
  }

  return ( *it ) -> power_type == ARTIFACT_TRAIT_RELIC ||
         ( *it ) -> power_type == ARTIFACT_TRAIT_MINOR2;
}

js::JsonOutput&& player_artifact_data_t::generate_report( js::JsonOutput&& root ) const
{
  if ( ! enabled() || points() == 0 )
  {
    return std::move( root );
  }

  root.make_array();

  auto order = powers();
  range::for_each( order, [ &root, this ]( const artifact_power_data_t* power ) {
    // No point found
    auto it = m_points.find( power -> id );
    if ( it == m_points.end() )
    {
      return;
    }

    // Unknown spell
    auto spell = player() -> dbc.spell( power -> power_spell_id );
    if ( spell == nullptr )
    {
      return;
    }

    auto node = root.add();

    node[ "id" ] = power -> id;
    node[ "spell_id" ] = spell -> id();
    node[ "name" ] = spell -> name_cstr();
    node[ "total_rank" ] = it -> second.purchased + it -> second.bonus + it -> second.crucible;
    node[ "purchased_rank" ] = it -> second.purchased;
    node[ "crucible_rank" ] = it -> second.crucible;
    node[ "relic_rank" ] = it -> second.bonus;
    if ( it -> second.overridden >= 0 )
    {
      node[ "override_rank" ] = it -> second.overridden;
    }
  } );

  return std::move( root );
}

report::sc_html_stream& player_artifact_data_t::generate_report( report::sc_html_stream& root ) const
{
  if ( ! enabled() || points() == 0 )
  {
    return root;
  }

  root << "<tr class=\"left\">\n<th>Artifact</th>\n<td><ul class=\"float\">\n";

  root << "<li><a href=\""
       << util::create_wowhead_artifact_url( *player() )
       << "\" target=\"_blank\">Calculator (Wowhead.com)</a></li>";

  root << "<li>Purchased points: " << purchased_points() << ", total " << points() << "</li>";
  root << "</ul></td></tr>";

  root << "<tr class=\"left\">\n<th></th><td><ul class=\"float\">\n";

  auto order = powers();
  range::for_each( order, [ &root, this ]( const artifact_power_data_t* power ) {
    // No point found
    auto it = m_points.find( power -> id );
    if ( it == m_points.end() )
    {
      return;
    }

    auto spell = player() -> dbc.spell( power -> power_spell_id );

    root << "<li>" << ( spell ? report::spell_data_decorator_t( player(), spell ).decorate()
                              : power -> name );

    auto purchased_rank = purchased_power_rank( power -> id );
    auto relic_rank = bonus_rank( power -> id );
    auto crucible_rank_ = crucible_rank( power -> id );

    if ( power -> max_rank > 1 && purchased_rank + relic_rank + crucible_rank_ > 0 )
    {
      std::stringstream rank_ss;
      if ( power -> power_type != ARTIFACT_TRAIT_RELIC )
      {
        rank_ss << purchased_rank;
        if ( relic_rank > 0 || ( relic_rank == 0 && crucible_rank_ > 0 ) )
        {
          rank_ss << " + " << relic_rank;
        }

        if ( crucible_rank_ > 0 )
        {
          rank_ss << " + " << crucible_rank_;
        }
      }
      else
      {
        rank_ss << crucible_rank_;
      }

      root << " (Rank " << rank_ss.str() << ")";
    }

    if ( it -> second.overridden >= 0 )
    {
      if ( purchased_rank == 0 )
      {
        root << " (<b>Disabled</b>)";
      }
      else
      {
        root << " (<b>Overridden</b>)";
      }
    }

    root << "</li>";
  } );

  root << "</ul></td>";

  root << "</tr>";

  return root;
}

bool player_artifact_data_t::parse()
{
  if ( ! enabled() )
  {
    return true;
  }

  if ( m_artifact_str.empty() )
  {
    return true;
  }

  if ( util::str_in_str_ci( m_artifact_str, ".wowdb.com/artifact-calculator#" ) )
  {
    error( this, "Player %s wowdb artifact calculator parsing is no longer supported",
      player() -> name() );
    return true;
  }

  auto splits = util::string_split( m_artifact_str, ":" );

  if ( splits.size() < 5 || ( splits.size() - 5 ) % 2 != 0 )
  {
    error( this, "Player %s invalid artifact format, "
                 "expected: artifact_id:relic1id:relic2id:relic3id:relic4id[:power_id:rank...]",
      player() -> name() );
    return false;
  }

  auto artifact_id = util::to_unsigned( splits[ 0 ] );
  if ( artifact_id == 0 )
  {
    error( this, "Player %s invalid artifact format, "
                 "expected: artifact_id:relic1id:relic2id:relic3id:relic4id[:power_id:rank...]",
      player() -> name() );
    return false;
  }

  auto spec_artifact_id = player() -> dbc.artifact_by_spec( player() -> specialization() );
  if ( spec_artifact_id != artifact_id )
  {
    error( this, "Player %s invalid artifact identifier '%u', expected '%u'",
      player() -> name(), artifact_id, spec_artifact_id );
    return false;
  }

  std::vector<unsigned> relics( MAX_ARTIFACT_RELIC );
  for ( size_t i = 1; i < 5; ++i )
  {
    relics[ i - 1 ] = util::to_unsigned( splits[ i ] );
  }

  auto artifact_powers = powers();
  for ( size_t i = 5; i < splits.size() - 1; i += 2 )
  {
    auto power_id = util::to_unsigned( splits[ i ] );
    auto rank = util::to_unsigned( splits[ i + 1 ] );
    if ( power_id == 0 )
    {
      error( this, "Player %s invalid artifact power '%s', expected a number > 0",
          player() -> name(), splits[ i ].c_str() );
      return false;
    }

    if ( rank == 0 || rank > artifact::MAX_TRAIT_RANK )
    {
      error( this, "Player %s invalid artifact rank '%s' for power '%u', expected a number > 0 && <= 255",
        player() -> name(), splits[ i + 1 ].c_str(), power_id );
      return false;
    }

    if ( ! valid_power( power_id ) )
    {
      error( this, "Player %s invalid artifact power id '%u' for specialization '%s'",
        player() -> name(), power_id, util::specialization_string( player() -> specialization() ) );
      return false;
    }

    auto power = this -> power( power_id );
    if ( power && power -> power_type == ARTIFACT_TRAIT_RELIC )
    {
      error( this, "Player %s artifact trait '%s' (%u) is a crucible trait. "
             "Use 'crucible' option instead",
        player() -> name(), power -> name, power -> id );
      return false;
    }

    add_power( power_id, rank );
  }

  // Adjust artifact points based on relic traits
  range::for_each( relics, [ & ]( unsigned item_id ) {
    if ( item_id == 0 )
    {
      return;
    }

    auto relic_data = player() -> dbc.artifact_relic_rank_index( artifact_id, item_id );
    if ( relic_data.first == 0 || relic_data.second == 0 )
    {
      return;
    }

    add_relic( item_id, relic_data.first, relic_data.second );
    // Wowhead includes the relic ranks in the rank information, so move the bonus points from
    // purchased ranks to the bonus ranks.
    move_purchased_rank( relic_data.first, relic_data.second );
  } );

  return true;
}

bool player_artifact_data_t::parse_crucible()
{
  if ( ! enabled() )
  {
    return true;
  }

  if ( m_crucible_str.empty() )
  {
    return true;
  }

  auto splits = util::string_split( m_crucible_str, ":" );

  if ( splits.size() % 2 != 0 )
  {
    error( this, "Player %s invalid crucible format, expected: power_id:rank[:power_id:rank...]",
      player() -> name() );
    return false;
  }

  auto artifact_powers = powers();
  for ( size_t i = 0; i < splits.size() - 1; i += 2 )
  {
    auto power_id = util::to_unsigned( splits[ i ] );
    auto rank = util::to_unsigned( splits[ i + 1 ] );
    if ( power_id == 0 )
    {
      error( this, "Player %s invalid crucible power '%s', expected a number > 0",
        player() -> name(), splits[ i ].c_str() );
      return false;
    }

    if ( rank == 0 || rank > artifact::MAX_TRAIT_RANK )
    {
      error( this, "Player %s invalid crucible rank '%s' for power '%u', expected a number > 0 && <= 255",
        player() -> name(), splits[ i + 1 ].c_str(), power_id );
      return false;
    }

    if ( ! valid_crucible_power( power_id ) )
    {
      error( this, "Player %s invalid crucible power id '%u'", player() -> name(), power_id );
      return false;
    }

    add_crucible_power( power_id, rank );
  }

  return true;
}
} // Namespace artifact ends
