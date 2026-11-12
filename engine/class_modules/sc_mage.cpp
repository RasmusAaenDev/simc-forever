// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
//
// AULA-RETTELSE (simc-worker/patches/mage.patch i AULA-Guild):
// Grov basismodel for Forever-mage, bygget efter warlock-modulets skabelon.
// Spells med rank efter spillerens level. Talenternes passive effekter laeses
// fra spildata (parse_all_passive_talents). Frost-pakken: Ice Lance, frossen
// tilstand (Frostbite), Fingers of Frost, Shatter og Ice Barrier. Fire-pakken:
// Scorch, Pyroblast, Ignite, Improved Scorch (Fire Vulnerability), Heating Up,
// Combustion, Master of Elements og Fire Power. Wand (Shoot) og Evocation til
// mana-styring. APL'en vaelger rotation ud fra de valgte talenter.
// Alle tal kommer fra spildata og vaabendata.
// Fjernes naar upstream selv implementerer klassen.

#include "simulationcraft.hpp"
#include "action/parse_effects.hpp"
#include "action/residual_action.hpp"

#include "class_modules/apl/mage.hpp"

namespace mage
{
struct mage_t;

// Maalets tilstand: "frozen" = Frostbite (12494, Root, 5 s) fra Frostbolts Chill;
// "fire_vulnerability" = Improved Scorch (22959, +3 % Fire-skade fra magen pr. stak, 5 stak, 30 s).
struct mage_td_t final : public actor_target_data_t
{
  struct
  {
    buff_t* frozen;
    buff_t* fire_vulnerability;
  } debuff;

  mage_td_t( player_t* target, mage_t& p );
};

struct mage_t final : public parse_player_effects_t
{
  mage_t( sim_t* sim, std::string_view name, race_e r = RACE_NONE ) :
    parse_player_effects_t( sim, MAGE, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_MANA; }

  std::string default_potion() const override { return mage_apl::potion( this ); }
  std::string default_flask() const override  { return mage_apl::flask( this ); }
  std::string default_food() const override   { return mage_apl::food( this ); }

  action_t* create_action( std::string_view name, std::string_view options_str ) override;
  void init_action_list() override;
  void init_base_stats() override;
  void init_spells() override;
  void create_buffs() override;
  void init_finished() override;
  void create_actions() override;
  void init_gains() override;

  target_specific_t<mage_td_t> target_data;

  mage_td_t* get_target_data( player_t* target ) const override
  {
    mage_td_t*& td = target_data[ target ];
    if ( !td )
      td = new mage_td_t( target, const_cast<mage_t&>( *this ) );
    return td;
  }

  // Frostbolts Chill ramte: Frostbite (frys) og Fingers of Frost kan udloeses.
  void trigger_chill( action_state_t* s );

  // Direkte skadeshit fra en af magens spells: Ignite, Combustion, Heating Up, Master of Elements.
  void trigger_direct_procs( action_t* a, action_state_t* s, bool heating_up_source );

  // Fire Power: antal af magens Fire-effekter paa maalet (hoejst 4).
  int fire_effects_on( player_t* t ) const;

  struct
  {
    action_t* ignite;
  } action;

  struct
  {
    gain_t* master_of_elements;
  } gains;

  struct
  {
    buff_t* evocation;
    buff_t* fingers_of_frost;
    buff_t* ice_barrier;
    buff_t* frigid_power;
    buff_t* heating_up;
    buff_t* combustion;       // 11129: tilbagevaerende crits (3)
    buff_t* combustion_crit;  // 28682: +10 % crit pr. stak paa Fire-spells
  } buff;

  struct
  {
    player_talent_t wand_specialization;
    player_talent_t frostbite;
    player_talent_t fingers_of_frost;
    player_talent_t shatter;
    player_talent_t ice_lance;
    player_talent_t ice_barrier;
    player_talent_t pyroblast;
    player_talent_t ignite;
    player_talent_t improved_scorch;
    player_talent_t heating_up;
    player_talent_t master_of_elements;
    player_talent_t fire_power;
    player_talent_t combustion;
  } talent;

  struct
  {
    spell_ranks_t arcane_missiles;
    spell_ranks_t evocation;
    spell_ranks_t fire_blast;
    spell_ranks_t fireball;
    spell_ranks_t frostbolt;
    spell_ranks_t ice_lance;
    spell_ranks_t ice_barrier;
    spell_ranks_t scorch;
    spell_ranks_t pyroblast;
    spell_ranks_t combustion;
  } rank;
};

inline mage_td_t::mage_td_t( player_t* target, mage_t& p ) : actor_target_data_t( target, &p )
{
  debuff.frozen = make_buff_fallback( p.talent.frostbite.ok() && target->is_enemy(), *this, "frozen", p.find_spell( 12494 ) );
  debuff.fire_vulnerability = make_buff_fallback( p.talent.improved_scorch.ok() && target->is_enemy(), *this,
                                                  "fire_vulnerability", p.find_spell( 22959 ) );
}

// MAGE MODULE INTERFACE ====================================================

struct mage_module_t final : public module_t
{
public:
  mage_module_t() :
    module_t( MAGE )
  { }

  player_t* create_player( sim_t* sim, std::string_view name, race_e r = RACE_NONE ) const override
  {
    auto p = new mage_t( sim, name, r );
    return p;
  }

  void register_hotfixes() const override {}

  bool valid() const override { return true; }

  void register_actor_initializers( sim_t* ) const override {}
};

template <class Base>
struct mage_action_t : public parse_action_effects_t<Base>
{
private:
  using ab = parse_action_effects_t<Base>;

public:
  using base_t = mage_action_t<Base>;

  template <typename... Args>
  mage_action_t( Args&&... args ) : ab( std::forward<Args>( args )... )
  {
    // Ice Barrier effekt 2 og Frigid Power effekt 1: Add Percent Modifier (Spell Direct Amount)
    // paa Frost-spells; virker pr. handling, mens buffen er oppe (som druidens Clearcasting).
    if ( this->data().ok() )
    {
      this->parse_effects( p()->buff.ice_barrier );
      this->parse_effects( p()->buff.frigid_power );
      // Heating Up (400625): -25 % cast-tid pr. stak paa Pyroblast. Combustion (28682):
      // +10 % crit pr. stak paa Fire-spells. Begge virker kun paa spildataens beroerte spells.
      this->parse_effects( p()->buff.heating_up );
      this->parse_effects( p()->buff.combustion_crit );
    }
  }

  mage_t* p()
  { return debug_cast<mage_t*>( this->player ); }

  const mage_t* p() const
  { return debug_cast<const mage_t*>( this->player ); }

  // Frossen-tilstand for denne cast: maalet er frosset (Frostbite), eller Fingers of Frost
  // behandler maalet som frosset. Afgoeres ved execute, hvor crit og skade beregnes.
  // Shoot (wand, auto-repeat) og spells paa magen selv er ikke omfattet.
  bool affected_by_frozen = true;
  bool frozen_cast = false;
  // Heating Up taeller ikke-periodiske crits fra Fireball, Fire Blast og Scorch.
  bool heating_up_source = false;

  bool target_frozen( player_t* t ) const
  { return p()->buff.fingers_of_frost->check() || p()->get_target_data( t )->debuff.frozen->check(); }

  void execute() override
  {
    bool use = affected_by_frozen && this->harmful && !this->background;
    bool fof = use && p()->buff.fingers_of_frost->check();
    frozen_cast = use && target_frozen( this->target );
    ab::execute();
    frozen_cast = false;
    // Fingers of Frost: "your next N spells" -> en ladning pr. kastet spell.
    if ( fof )
      p()->buff.fingers_of_frost->decrement();
  }

  // Direkte hit (ikke DoT-ticks): Ignite, Combustion, Heating Up og Master of Elements.
  void impact( action_state_t* s ) override
  {
    ab::impact( s );
    if ( this->harmful && s->result_type == result_amount_type::DMG_DIRECT && this->result_is_hit( s->result ) )
      p()->trigger_direct_procs( this, s, heating_up_source );
  }

  // Improved Scorch (Fire Vulnerability, +3 % pr. stak) og Fire Power (+1-3 % pr. Fire-effekt
  // paa maalet, hoejst 4) gaelder al magens Fire-skade, direkte og periodisk.
  double composite_target_multiplier( player_t* t ) const override
  {
    double m = ab::composite_target_multiplier( t );
    if ( this->get_school() != SCHOOL_FIRE || !this->harmful )
      return m;
    if ( p()->talent.improved_scorch.ok() )
    {
      const buff_t* fv = p()->get_target_data( t )->debuff.fire_vulnerability;
      if ( fv->check() )
        m *= 1.0 + fv->check() * fv->data().effectN( 1 ).percent();
    }
    if ( p()->talent.fire_power.ok() )
    {
      int n = p()->fire_effects_on( t );
      if ( n )
        m *= 1.0 + n * p()->talent.fire_power->effectN( 1 ).percent();
    }
    return m;
  }

  // Shatter (11170): +17/33/50 % crit (talentkurven) for alle spells mod frosne maal.
  double composite_target_crit_chance( player_t* t ) const override
  {
    double c = ab::composite_target_crit_chance( t );
    if ( frozen_cast && p()->talent.shatter.ok() )
      c += p()->talent.shatter->effectN( 1 ).percent();
    return c;
  }
};

using mage_spell_t = mage_action_t<spell_t>;

// Frostbolt
struct frostbolt_t : public mage_spell_t
{
  frostbolt_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "frostbolt", player, player->rank.frostbolt.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  // Frostbolts effekt 1 (Decrease Movement Speed) er en Chill-effekt; den lander kun ved hit.
  void impact( action_state_t* s ) override
  {
    mage_spell_t::impact( s );
    if ( result_is_hit( s->result ) )
      p()->trigger_chill( s );
  }
};

// Ice Lance (talent 1312002; hoejere ranks 400640, 1240044-1240047 kraever talentet).
// Instant, 8 s nedkoeling; effekt 2: +300 % skade mod frosne maal. Spildataen har ingen
// spell power-koefficient paa skadeeffekten, saa skaden er kun rankens basisvaerdi.
struct ice_lance_t : public mage_spell_t
{
  ice_lance_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "ice_lance", player, player->rank.ice_lance.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  double composite_target_da_multiplier( player_t* t ) const override
  {
    double m = mage_spell_t::composite_target_da_multiplier( t );
    if ( frozen_cast )
      m *= 1.0 + data().effectN( 2 ).percent();
    return m;
  }
};

// Ice Barrier (talent 11426, ranks 13031-13033): skjold paa magen selv i 60 s. Effekt 2
// (+5 % direkte Frost-skade, mens skjoldet holder) ligger paa buffen; beskrivelsen siger
// desuden Frigid Power (1326891: +15 % Frost-skade i 60 s). Absorberingen modelleres ikke
// (magen tager ingen skade), saa skjoldet holder hele varigheden.
struct ice_barrier_t : public mage_spell_t
{
  ice_barrier_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "ice_barrier", player, player->rank.ice_barrier.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = false;
    target = player;
    // Effekt 1 (Absorb Damage) er ikke skade; uden dette taeller simc den som direkte skade.
    base_dd_min = base_dd_max = 0.0;
  }

  void execute() override
  {
    mage_spell_t::execute();
    p()->buff.ice_barrier->trigger();
    p()->buff.frigid_power->trigger();
  }
};

// Fireball (direkte skade + DoT, begge fra spildata)
struct fireball_t : public mage_spell_t
{
  fireball_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "fireball", player, player->rank.fireball.find_rank()->spell() )
  {
    parse_options( options_str );
    heating_up_source = true;
  }
};

// Fire Blast (instant; nedkoeling fra spildata)
struct fire_blast_t : public mage_spell_t
{
  fire_blast_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "fire_blast", player, player->rank.fire_blast.find_rank()->spell() )
  {
    parse_options( options_str );
    heating_up_source = true;
  }

  // Heating Up: "Fire Blast grants an additional stack every time it deals damage" (ud over crit-stakken).
  void impact( action_state_t* s ) override
  {
    mage_spell_t::impact( s );
    if ( p()->talent.heating_up.ok() && result_is_hit( s->result ) )
      p()->buff.heating_up->trigger();
  }
};

// Scorch (traeneren, level 22+): 1,5 s cast. Improved Scorch (11095): 33/67/100 % chance pr. hit
// for en stak Fire Vulnerability (22959) paa maalet.
struct scorch_t : public mage_spell_t
{
  scorch_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "scorch", player, player->rank.scorch.find_rank()->spell() )
  {
    parse_options( options_str );
    heating_up_source = true;
  }

  void impact( action_state_t* s ) override
  {
    mage_spell_t::impact( s );
    if ( p()->talent.improved_scorch.ok() && result_is_hit( s->result ) &&
         rng().roll( p()->talent.improved_scorch->effectN( 1 ).percent() ) )
      p()->get_target_data( s->target )->debuff.fire_vulnerability->trigger();
  }
};

// Pyroblast (talent 11366; hoejere ranks kraever talentet): 6 s cast, direkte skade + DoT
// (4 ticks a 3 s), alt fra spildata. Heating Up-stakkene (-25 % cast-tid hver) bruges op.
struct pyroblast_t : public mage_spell_t
{
  pyroblast_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "pyroblast", player, player->rank.pyroblast.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  void execute() override
  {
    mage_spell_t::execute();
    p()->buff.heating_up->expire();
  }
};

// Combustion (talent 11129): hvert Fire-hit giver +10 % crit (28682, stakker), indtil magen har
// lavet 3 ikke-periodiske Fire-crits. Ingen GCD; 180 s nedkoeling, der ifoelge spildata
// ("Cooldown On Event") foerst starter, naar effekten er brugt op (se trigger_direct_procs).
struct combustion_t : public mage_spell_t
{
  combustion_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "combustion", player, player->rank.combustion.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = false;
    target = player;
  }

  bool ready() override
  {
    if ( p()->buff.combustion->check() )
      return false;
    return mage_spell_t::ready();
  }

  void update_ready( timespan_t ) override {}

  void execute() override
  {
    mage_spell_t::execute();
    p()->buff.combustion->trigger( p()->buff.combustion->max_stack() );
  }
};

// Ignite (talent 11119): Fire-crits braender maalet for 8-40 % af crittets skade over 4 s
// (412538, tick hver 2 s). Rullende: ny Ignite laegges oven i resten af den gamle. Skaden er
// en fast andel af crittet, saa ingen multiplikatorer laegges paa igen (antagelse).
struct ignite_t : public residual_action::residual_periodic_action_t<spell_t>
{
  ignite_t( mage_t* player ) : base_t( "ignite", player, player->find_spell( 412538 ) )
  {
    dot_duration   = data().duration();
    base_tick_time = data().effectN( 1 ).period();
    hasted_ticks = tick_may_crit = may_miss = false;
    school = SCHOOL_FIRE;
  }

  void init() override
  {
    base_t::init();
    snapshot_flags = update_flags = 0;
  }
};

// Arcane Missiles: kanaliseret driver; hvert tick udloeser et Arcane Missile (spildataens trigger-spell)
struct arcane_missile_t : public mage_spell_t
{
  arcane_missile_t( mage_t* player ) :
    mage_spell_t( "arcane_missile", player,
                  player->rank.arcane_missiles.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->attack() )
  {
    background = dual = true;
  }
};

struct arcane_missiles_t : public mage_spell_t
{
  arcane_missiles_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "arcane_missiles", player,
                  player->rank.arcane_missiles.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->spell() )
  {
    parse_options( options_str );
    channeled = true;
    may_miss = may_crit = false;
    tick_action = new arcane_missile_t( player );
  }
};

// Shoot (wand, 5019): et skud med den udstyrede wand i ranged-pladsen. Spildataen siger
// Weapon Damage (NoSchool) og Spell Type Magic: skaden er wandens vaabenskade, skolen kommer
// fra wanden (staar ikke i simcs item-data), og skuddet bruger spell-tabellen (hit/crit).
// Hvert skud modelleres som en "cast" af wandens hastighed (auto-repeat afbrydes i spillet af
// andre spells), saa APL'en kan vende tilbage til Frostbolt naar manaen er der igen.
// Uden wand i ranged-pladsen er handlingen aldrig klar.
struct shoot_t : public mage_spell_t
{
  shoot_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "shoot", player, player->find_spell( 5019 ) )
  {
    parse_options( options_str );
    affected_by_frozen = false;

    weapon            = &player->equipped_weapons[ SLOT_RANGED ];
    weapon_multiplier = 1.0;
    base_execute_time = weapon->swing_time;
    may_crit          = true;
    // Wandens egen skole, hvis profilen angiver den (weapon=wand_..._shadow); ellers spildataens.
    if ( weapon->school != SCHOOL_PHYSICAL && weapon->school != SCHOOL_NONE )
      school = weapon->school;
  }

  bool has_wand() const
  { return weapon && weapon->type == WEAPON_WAND && weapon->max_dmg > 0; }

  // Wand-skade er magisk (Spell Type Magic); rustning gaelder ikke, selv om skolen er ukendt.
  double composite_target_armor( const action_state_t* ) const override
  { return 0.0; }

  // Wandens hastighed, ikke spell-haste.
  timespan_t execute_time() const override
  { return base_execute_time; }

  // Wand Specialization: +13/25 % wand-skade (rank-vaerdien fra spildataens kurve).
  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = mage_spell_t::composite_da_multiplier( s );
    if ( p()->talent.wand_specialization.ok() )
      m *= 1.0 + p()->talent.wand_specialization->effectN( 1 ).percent();
    return m;
  }

  bool ready() override
  {
    if ( !has_wand() )
      return false;
    return mage_spell_t::ready();
  }
};

// Evocation (12051): kanaliseret paa magen selv i 8 s. Effekterne (Modify Power Regen% +1500 %
// og Allow Mana Regen While Casting 100 %) ligger paa buffen og laeses af parse_player_effects.
struct evocation_t : public mage_spell_t
{
  evocation_t( mage_t* player, std::string_view options_str ) :
    mage_spell_t( "evocation", player, player->rank.evocation.find_rank()->spell() )
  {
    parse_options( options_str );

    channeled = true;
    harmful = may_miss = may_crit = false;
    target = player;
  }

  void execute() override
  {
    mage_spell_t::execute();
    p()->buff.evocation->trigger( composite_dot_duration( execute_state ) );
  }

  void last_tick( dot_t* d ) override
  {
    mage_spell_t::last_tick( d );
    p()->buff.evocation->expire();
  }
};

action_t* mage_t::create_action( std::string_view name, std::string_view options_str )
{
  if ( name == "frostbolt" )
    return new frostbolt_t( this, options_str );
  if ( name == "fireball" )
    return new fireball_t( this, options_str );
  if ( name == "fire_blast" )
    return new fire_blast_t( this, options_str );
  if ( name == "arcane_missiles" )
    return new arcane_missiles_t( this, options_str );
  if ( name == "evocation" )
    return new evocation_t( this, options_str );
  if ( name == "shoot" )
    return new shoot_t( this, options_str );
  if ( name == "ice_lance" )
    return new ice_lance_t( this, options_str );
  if ( name == "ice_barrier" )
    return new ice_barrier_t( this, options_str );
  if ( name == "scorch" )
    return new scorch_t( this, options_str );
  if ( name == "pyroblast" )
    return new pyroblast_t( this, options_str );
  if ( name == "combustion" )
    return new combustion_t( this, options_str );

  return parse_player_effects_t::create_action( name, options_str );
}

void mage_t::init_base_stats()
{
  // Ingen egne konstanter: basis-crit m.m. kommer fra spildata via player_t.
  parse_player_effects_t::init_base_stats();
}

void mage_t::create_buffs()
{
  parse_player_effects_t::create_buffs();

  auto const evocation = rank.evocation.find_rank()->spell();
  buff.evocation = make_buff_fallback( evocation->ok(), this, "evocation", evocation )
                     ->set_cooldown( 0_ms )
                     ->set_affects_regen( true );

  // Fingers of Frost (400647 -> 400669, 15 s): ladninger = talentets effekt 1 (1/2 efter rank).
  buff.fingers_of_frost = make_buff_fallback( talent.fingers_of_frost.ok(), this, "fingers_of_frost", find_spell( 400669 ) );
  if ( talent.fingers_of_frost.ok() )
    buff.fingers_of_frost->set_max_stack( as<int>( talent.fingers_of_frost->effectN( 1 ).base_value() ) );

  auto const ice_barrier = rank.ice_barrier.find_rank()->spell();
  buff.ice_barrier = make_buff_fallback( ice_barrier->ok(), this, "ice_barrier", ice_barrier )
                       ->set_cooldown( 0_ms );
  buff.frigid_power = make_buff_fallback( ice_barrier->ok(), this, "frigid_power", find_spell( 1326891 ) );

  // Heating Up (400625): 20 s, op til 3 stak, -25 % cast-tid paa Pyroblast pr. stak.
  buff.heating_up = make_buff_fallback( talent.heating_up.ok(), this, "heating_up", find_spell( 400625 ) );

  // Combustion: taelleren (11129, 3 crits) og crit-stakken (28682, +10 % pr. stak, op til 10).
  auto const combustion = rank.combustion.find_rank()->spell();
  buff.combustion = make_buff_fallback( combustion->ok(), this, "combustion", combustion )->set_cooldown( 0_ms );
  buff.combustion_crit = make_buff_fallback( combustion->ok(), this, "combustion_crit", find_spell( 28682 ) );
}

int mage_t::fire_effects_on( player_t* t ) const
{
  int n = get_target_data( t )->debuff.fire_vulnerability->check() ? 1 : 0;
  for ( auto name : { "fireball", "pyroblast", "ignite" } )
  {
    dot_t* d = t->find_dot( name, const_cast<mage_t*>( this ) );
    if ( d && d->is_ticking() )
      n++;
  }
  return std::min( n, 4 );
}

void mage_t::trigger_direct_procs( action_t* a, action_state_t* s, bool heating_up_source )
{
  school_e school = a->get_school();
  bool crit = s->result == RESULT_CRIT;

  if ( school == SCHOOL_FIRE )
  {
    // Ignite: 8-40 % af crittets skade som rullende DoT.
    if ( crit && talent.ignite.ok() )
      residual_action::trigger( action.ignite, s->target, s->result_amount * talent.ignite->effectN( 1 ).percent() );

    // Heating Up: ikke-periodiske crits med Fireball, Fire Blast og Scorch.
    if ( crit && heating_up_source && talent.heating_up.ok() )
      buff.heating_up->trigger();

    // Combustion: et crit bruger en af de 3; hvert hit, mens Combustion er aktiv, giver en crit-stak.
    if ( buff.combustion->check() )
    {
      if ( crit )
        buff.combustion->decrement();
      if ( buff.combustion->check() )
        buff.combustion_crit->trigger();
      else
      {
        buff.combustion_crit->expire();
        get_cooldown( "combustion" )->start( rank.combustion.find_rank()->spell()->cooldown() );
      }
    }
  }

  // Master of Elements: Fire- og Frost-crits giver 10/20/30 % af spellens basis-mana tilbage.
  if ( crit && talent.master_of_elements.ok() && ( school == SCHOOL_FIRE || school == SCHOOL_FROST ) )
    resource_gain( RESOURCE_MANA, a->base_costs[ RESOURCE_MANA ].base * talent.master_of_elements->effectN( 1 ).percent(),
                   gains.master_of_elements, a );
}

void mage_t::trigger_chill( action_state_t* s )
{
  // Frostbite (11071): 5/10/15 % chance for at fryse maalet (12494, 5 s).
  if ( talent.frostbite.ok() && rng().roll( talent.frostbite->effectN( 1 ).percent() ) )
    get_target_data( s->target )->debuff.frozen->trigger();

  // Fingers of Frost (400647): effekt 2 = 15 % chance; buffen faar fulde ladninger.
  if ( talent.fingers_of_frost.ok() && rng().roll( talent.fingers_of_frost->effectN( 2 ).percent() ) )
    buff.fingers_of_frost->trigger( buff.fingers_of_frost->max_stack() );
}

void mage_t::create_actions()
{
  action.ignite = talent.ignite.ok() ? new ignite_t( this ) : nullptr;
  parse_player_effects_t::create_actions();
}

void mage_t::init_gains()
{
  parse_player_effects_t::init_gains();
  gains.master_of_elements = talent.master_of_elements.ok() ? get_gain( "master_of_elements" ) : nullptr;
}

void mage_t::init_finished()
{
  // Evocations regen-effekter (aura 110 og 134) virker gennem buffen.
  parse_effects( buff.evocation );

  parse_player_effects_t::init_finished();
}

void mage_t::init_spells()
{
  parse_player_effects_t::init_spells();

  talent.frostbite        = find_talent_spell( talent_tree::CLASS, "Frostbite" );
  talent.fingers_of_frost = find_talent_spell( talent_tree::CLASS, "Fingers of Frost" );
  talent.shatter          = find_talent_spell( talent_tree::CLASS, "Shatter" );
  talent.ice_lance        = find_talent_spell( talent_tree::CLASS, "Ice Lance" );
  talent.ice_barrier      = find_talent_spell( talent_tree::CLASS, "Ice Barrier" );

  talent.pyroblast          = find_talent_spell( talent_tree::CLASS, "Pyroblast" );
  talent.ignite             = find_talent_spell( talent_tree::CLASS, "Ignite" );
  talent.improved_scorch    = find_talent_spell( talent_tree::CLASS, "Improved Scorch" );
  talent.heating_up         = find_talent_spell( talent_tree::CLASS, "Heating Up" );
  talent.master_of_elements = find_talent_spell( talent_tree::CLASS, "Master of Elements" );
  talent.fire_power         = find_talent_spell( talent_tree::CLASS, "Fire Power" );
  talent.combustion         = find_talent_spell( talent_tree::CLASS, "Combustion" );

  rank.pyroblast = { this, talent.pyroblast,
    spell_container_t().set_spell( 11366 ),  // 20
    spell_container_t().set_spell( 12505 ),  // 24
    spell_container_t().set_spell( 12522 ),  // 30
    spell_container_t().set_spell( 12523 ),  // 36
    spell_container_t().set_spell( 12524 ),  // 42
    spell_container_t().set_spell( 12525 ),  // 48
    spell_container_t().set_spell( 12526 ),  // 54
    spell_container_t().set_spell( 18809 ),  // 60
  };

  rank.combustion = { this, talent.combustion,
    spell_container_t().set_spell( 11129 ),  // 40
  };

  rank.scorch = { this,
    spell_container_t().set_spell(  2948 ),  // 22
    spell_container_t().set_spell(  8444 ),  // 28
    spell_container_t().set_spell(  8445 ),  // 34
    spell_container_t().set_spell(  8446 ),  // 40
    spell_container_t().set_spell( 10205 ),  // 46
    spell_container_t().set_spell( 10206 ),  // 52
    spell_container_t().set_spell( 10207 ),  // 58
  };

  // Talent-spells: rank 1 = talentet; hoejere ranks kraever talentet (kommentar = spell level).
  rank.ice_lance = { this, talent.ice_lance,
    spell_container_t().set_spell( 1312002 ),  // 20
    spell_container_t().set_spell(  400640 ),  // 28
    spell_container_t().set_spell( 1240044 ),  // 34
    spell_container_t().set_spell( 1240045 ),  // 42
    spell_container_t().set_spell( 1240046 ),  // 48
    spell_container_t().set_spell( 1240047 ),  // 56
  };

  rank.ice_barrier = { this, talent.ice_barrier,
    spell_container_t().set_spell( 11426 ),  // 40
    spell_container_t().set_spell( 13031 ),  // 46
    spell_container_t().set_spell( 13032 ),  // 52
    spell_container_t().set_spell( 13033 ),  // 58
  };

  rank.evocation = { this,
    spell_container_t().set_spell( 12051 ),  // 20
  };

  // spell = kanaliseret driver, attack = Arcane Missile (udloest pr. tick); kommentar = spell level
  rank.arcane_missiles = { this,
    spell_container_t().set_spell(  5143 ).set_attack(  7268 ),  // 8
    spell_container_t().set_spell(  5144 ).set_attack(  7269 ),  // 16
    spell_container_t().set_spell(  5145 ).set_attack(  7270 ),  // 24
    spell_container_t().set_spell(  8416 ).set_attack(  8419 ),  // 32
    spell_container_t().set_spell(  8417 ).set_attack(  8418 ),  // 40
    spell_container_t().set_spell( 10211 ).set_attack( 10273 ),  // 48
    spell_container_t().set_spell( 10212 ).set_attack( 10274 ),  // 56
    spell_container_t().set_spell( 25345 ).set_attack( 25346 ),  // 56 / 60
  };

  rank.fire_blast = { this,
    spell_container_t().set_spell(  2136 ),  // 6
    spell_container_t().set_spell(  2137 ),  // 14
    spell_container_t().set_spell(  2138 ),  // 22
    spell_container_t().set_spell(  8412 ),  // 30
    spell_container_t().set_spell(  8413 ),  // 38
    spell_container_t().set_spell( 10197 ),  // 46
    spell_container_t().set_spell( 10199 ),  // 54
  };

  rank.fireball = { this,
    spell_container_t().set_spell(   133 ),  // 1
    spell_container_t().set_spell(   143 ),  // 6
    spell_container_t().set_spell(   145 ),  // 12
    spell_container_t().set_spell(  3140 ),  // 18
    spell_container_t().set_spell(  8400 ),  // 24
    spell_container_t().set_spell(  8401 ),  // 30
    spell_container_t().set_spell(  8402 ),  // 36
    spell_container_t().set_spell( 10148 ),  // 42
    spell_container_t().set_spell( 10149 ),  // 48
    spell_container_t().set_spell( 10150 ),  // 54
    spell_container_t().set_spell( 10151 ),  // 60
    spell_container_t().set_spell( 25306 ),  // 60
  };

  rank.frostbolt = { this,
    spell_container_t().set_spell(   116 ),  // 4
    spell_container_t().set_spell(   205 ),  // 8
    spell_container_t().set_spell(   837 ),  // 14
    spell_container_t().set_spell(  7322 ),  // 20
    spell_container_t().set_spell(  8406 ),  // 26
    spell_container_t().set_spell(  8407 ),  // 32
    spell_container_t().set_spell(  8408 ),  // 38
    spell_container_t().set_spell( 10179 ),  // 44
    spell_container_t().set_spell( 10180 ),  // 50
    spell_container_t().set_spell( 10181 ),  // 56
    spell_container_t().set_spell( 25304 ),  // 60
  };

  parse_all_class_passives();
  // Talenternes passive effekter direkte fra spildata (som warlock, priest, shaman m.fl.):
  // Add Flat/Percent Modifier paa de beroerte spells (fx Improved Frostbolt, Ice Shards,
  // Arcane Impact) og spillerstats (fx Precision, Arcane Mind, Arcane Instability).
  parse_all_passive_talents();

  // Wand Specialization (6057) er i spildata Modify Damage Done% for alle magiske skoler med
  // kravet "Requires weapon: Wand", som simc ikke ser; registreret ville den give ALLE spells
  // bonussen. Den fjernes derfor (som i priest-modulet) og laegges kun paa Shoot.
  talent.wand_specialization = find_talent_spell( talent_tree::CLASS, "Wand Specialization" );
  deregister_passive_spell( talent.wand_specialization );
}

void mage_t::init_action_list()
{
  if ( !action_list_str.empty() )
  {
    parse_player_effects_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  mage_apl::mage( this );
  use_default_action_list = true;
  parse_player_effects_t::init_action_list();
}

}  // namespace mage

const module_t* module_t::mage()
{
  static mage::mage_module_t m;
  return &m;
}
