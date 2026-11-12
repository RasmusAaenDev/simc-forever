//==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"
#include "action/parse_effects.hpp"
#include "action/residual_action.hpp"
#include "class_modules/apl/hunter.hpp"

// TODO: ItemDamageRanged for ranged weapon damage multipliers
// TODO: Does "plain" +Attack Power contribute to RAP?

namespace
{
namespace pets
{
struct pet_family_t;
struct hunter_pet_t;
}

// =========================================================================
// Hunter
// =========================================================================

struct hunter_t;

struct hunter_td_t final : public actor_target_data_t
{
  struct debuffs_t
  {
    buff_t* hunters_mark = nullptr;
  } debuff;

  hunter_td_t( player_t& target, hunter_t& p );
};

struct hunter_t final : public parse_player_effects_t
{
  using base_t = parse_player_effects_t;

  hunter_t( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) :
    base_t( sim, HUNTER, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_MANA; }

  void init_spells() override;
  void init_base_stats() override;
  void init_action_list() override;
  void init_finished() override;
  void create_options() override;
  void create_pets() override;
  std::unique_ptr<expr_t> create_expression( std::string_view name ) override;

  action_t* create_action( util::string_view name, util::string_view options_str ) override;
  void create_buffs() override;

  double composite_player_multiplier( school_e ) const override;
  double composite_melee_auto_attack_speed() const override;
  double composite_ranged_auto_attack_speed() const override;

  void parse_player_effects() {}
  void apply_action_effects( action_t* ) {}

  target_specific_t<hunter_td_t> target_data;

  hunter_td_t* get_target_data( player_t* target ) const override
  {
    assert( target );
    hunter_td_t*& td = target_data[ target ];
    if ( !td )
      td = new hunter_td_t( *target, const_cast<hunter_t&>( *this ) );
    return td;
  }

  const hunter_td_t* find_target_data( const player_t* target ) const
  { return target_data[ target ]; }

  // summon_pet=auto|none|<family>. auto: a cat, or no pet with Lone Wolf.
  std::string summon_pet_str = "auto";
  const pets::pet_family_t* pet_family = nullptr;
  pets::hunter_pet_t* pet = nullptr;

  bool pet_active() const;
  bool melee_spec() const;
  void delay_auto_shot( timespan_t cast_time );

  struct buffs_t
  {
    buff_t* aspect_of_the_hawk = nullptr;
    buff_t* trueshot_aura = nullptr;
    buff_t* rapid_fire = nullptr;
    buff_t* raptor_strike_queued = nullptr;
    buff_t* expose_prey = nullptr;
  } buff;

  struct
  {
    // Beast Mastery
    player_talent_t focused_fire;
    player_talent_t unleashed_fury;
    player_talent_t ferocity;
    player_talent_t bestial_discipline;
    player_talent_t frenzy;
    player_talent_t bestial_wrath;
    // Marksmanship
    player_talent_t lone_wolf;
    player_talent_t trueshot_aura;
    player_talent_t sniper_shot;
    // Survival
    player_talent_t savage_strikes;
    player_talent_t predators_edge;
    player_talent_t counterattack;
    player_talent_t expose_prey;
    player_talent_t strider_kick;
    player_talent_t lacerating_strikes;
  } talent;

  struct
  {
    spell_ranks_t aspect_of_the_hawk;
    spell_ranks_t arcane_shot;
    spell_ranks_t serpent_sting;
    spell_ranks_t hunters_mark;
    spell_ranks_t multi_shot;
    spell_ranks_t aimed_shot;
    spell_ranks_t rapid_fire;
    spell_ranks_t raptor_strike;
    spell_ranks_t mongoose_bite;
    spell_ranks_t trueshot_aura;
    spell_ranks_t sniper_shot;
    // Pet abilities, taught by the hunter's level
    spell_ranks_t bite;
    spell_ranks_t claw;
  } rank;

  struct
  {
    action_t* raptor_strike = nullptr;
    action_t* lacerating_strikes = nullptr;
  } active;

  cooldown_t* aimed_multi_shot = nullptr;
};

hunter_td_t::hunter_td_t( player_t& target, hunter_t& p ) : actor_target_data_t( &target, &p )
{
  auto mark = p.rank.hunters_mark.find_rank()->spell();
  // Effect 2: ranged attack power for every attacker against the target.
  debuff.hunters_mark = make_buff_fallback( target.is_enemy() && mark->ok(), *this, "hunters_mark", mark )
                            ->set_default_value( mark->effectN( 2 ).base_value() );
}

namespace pets
{
// =========================================================================
// Hunter pet
// =========================================================================

// A tamed beast's family. Every family shares the pet model below; the family's Tamed Pet Passive changes the
// damage it deals, and its abilities are Bite and/or Claw (the other family abilities are not modelled).
// TODO: The Tamed Pet Passives (17206-17223, 7000, 444831) are missing from the generated spell data (issue #5),
// so the damage modifiers are copied from the game files here.
struct pet_family_t
{
  std::string_view name;
  pet_e type;
  double damage_pct;
  bool bite;
  bool claw;
};

static constexpr pet_family_t pet_families[] = {
  { "cat",          PET_CAT,          0.10, true,  true  },
  { "raptor",       PET_RAPTOR,       0.10, true,  true  },
  { "bat",          PET_BAT,          0.07, true,  false },
  { "bird_of_prey", PET_BIRD_OF_PREY, 0.07, false, true  },
  { "spider",       PET_SPIDER,       0.07, true,  false },
  { "wind_serpent", PET_WIND_SERPENT, 0.07, true,  false },
  { "gorilla",      PET_GORILLA,      0.02, true,  false },
  { "core_hound",   PET_CORE_HOUND,   0.01, true,  false },
  { "carrion_bird", PET_CARRION_BIRD, 0.00, true,  true  },
  { "crocolisk",    PET_CROCOLISK,    0.00, true,  false },
  { "hyena",        PET_HYENA,        0.00, true,  false },
  { "tallstrider",  PET_TALLSTRIDER,  0.00, true,  false },
  { "wolf",         PET_WOLF,         0.00, true,  false },
  { "fox",          PET_FOX,          0.00, true,  false },
  { "crab",         PET_CRAB,        -0.05, false, true  },
  { "scorpid",      PET_SCORPID,     -0.06, false, true  },
  { "bear",         PET_BEAR,        -0.09, true,  true  },
  { "boar",         PET_BOAR,        -0.10, true,  false },
  { "turtle",       PET_TURTLE,      -0.10, true,  false },
};

const pet_family_t* find_pet_family( std::string_view name )
{
  for ( const auto& f : pet_families )
    if ( util::str_compare_ci( f.name, name ) )
      return &f;
  return nullptr;
}

// The pet's auto attack deals the creature auto attack damage of its level (expected_stat creature_auto_attack_dps,
// 60.45 at level 60) with its own attack power in it: its own attack power is 30% of that damage, at 14 attack
// power per point (4.2 x creature dps, which matches pet sheets at level 11, 17 and 30), so the 2.0 s swing before
// attack power is 70% of the creature damage, spanning 20% either side. On top of that the pet takes 10% of the
// hunter's higher attack power (melee or ranged), the hunter's crit and hit, and is always Happy (+25% damage).
// Numbers from wowsims/forever sim/hunter/pets.go (MIT).
struct hunter_pet_t final : public pet_t
{
  const pet_family_t& family;
  double own_attack_power = 0;

  struct
  {
    buff_t* bestial_wrath = nullptr;
    buff_t* frenzy = nullptr;
  } buffs;

  hunter_pet_t( hunter_t* owner, const pet_family_t& f ) :
    pet_t( owner->sim, owner, f.name, f.type, false, false ), family( f )
  {
    owner_coeff.ap_from_ap = 0.1;
    owner_coeff.armor = 0.3;
    stamina_per_owner = 0;

    equipped_weapons[ SLOT_MAIN_HAND ].type       = WEAPON_BEAST;
    equipped_weapons[ SLOT_MAIN_HAND ].swing_time = 2_s;
  }

  hunter_t* o() const
  { return debug_cast<hunter_t*>( owner ); }

  resource_e primary_resource() const override { return RESOURCE_FOCUS; }

  void init_base_stats() override
  {
    pet_t::init_base_stats();

    double dps = dbc->expected_stat( level() ).creature_auto_attack_dps;
    own_attack_power = 0.3 * 14 * dps;

    double swing = ( dps - own_attack_power / 14 ) * equipped_weapons[ SLOT_MAIN_HAND ].swing_time.total_seconds();
    equipped_weapons[ SLOT_MAIN_HAND ].min_dmg = swing * 0.8;
    equipped_weapons[ SLOT_MAIN_HAND ].max_dmg = swing * 1.2;
    equipped_weapons[ SLOT_MAIN_HAND ].damage  = swing;

    // Focus: 100, regenerating 10 a second; Bestial Discipline raises the regeneration.
    resources.base[ RESOURCE_FOCUS ] = 100;
    resources.base_regen_per_second[ RESOURCE_FOCUS ] = 10;
    if ( o()->talent.bestial_discipline.ok() )
      resources.base_regen_per_second[ RESOURCE_FOCUS ] *= 1.0 + o()->talent.bestial_discipline->effectN( 1 ).percent();
  }

  void create_buffs() override
  {
    pet_t::create_buffs();

    buffs.bestial_wrath = make_buff_fallback( o()->talent.bestial_wrath.ok(), this, "bestial_wrath",
                                              o()->talent.bestial_wrath )
                              ->set_default_value_from_effect( 2 )
                              ->set_cooldown( 0_ms );

    buffs.frenzy = make_buff_fallback( o()->talent.frenzy.ok(), this, "frenzy", o()->find_spell( 19615 ) )
                       ->set_default_value_from_effect( 1 )
                       ->add_invalidate( CACHE_AUTO_ATTACK_SPEED );
  }

  // The hunter's higher attack power, melee or ranged (Hunter Pet Scaling 415429).
  void update_stats() override
  {
    pet_t::update_stats();
    current_pet_stats.attack_power_from_ap =
        owner_coeff.ap_from_ap * std::max( owner->composite_total_attack_power_by_type( attack_power_type::NO_WEAPON ),
                                           owner->composite_total_attack_power_by_type( attack_power_type::RANGED ) );
  }

  double composite_melee_attack_power() const override
  { return own_attack_power + pet_t::composite_melee_attack_power(); }

  // The hunter's ranged hit; no expertise.
  double composite_melee_hit() const override
  { return owner->cache.attack_hit(); }

  double composite_melee_expertise( const weapon_t* ) const override
  { return 0.0; }

  double pet_crit() const override
  {
    double c = pet_t::pet_crit();
    if ( o()->talent.ferocity.ok() )
      c += o()->talent.ferocity->effectN( 1 ).percent();
    return c;
  }

  // Only Rapid Fire and Frenzy speed the pet up: the hunter's quiver and ranged haste do not reach it.
  double composite_melee_haste() const override
  { return 1.0; }

  double composite_melee_auto_attack_speed() const override
  {
    double s = 1.0;
    if ( o()->buff.rapid_fire->check() )
      s /= 1.0 + o()->buff.rapid_fire->data().effectN( 4 ).percent();
    if ( buffs.frenzy->check() )
      s /= 1.0 + buffs.frenzy->check_value();
    return s;
  }

  void adjust_auto_attack( gcd_haste_type ht ) override
  { player_t::adjust_auto_attack( ht ); }

  double composite_player_multiplier( school_e school ) const override
  {
    double m = pet_t::composite_player_multiplier( school );

    m *= 1.25;  // Happy
    m *= 1.0 + family.damage_pct;

    if ( o()->talent.unleashed_fury.ok() )
      m *= 1.0 + o()->talent.unleashed_fury->effectN( 1 ).percent();
    if ( o()->talent.focused_fire.ok() )
      m *= 1.0 + o()->talent.focused_fire->effectN( 1 ).percent();
    if ( buffs.bestial_wrath->check() )
      m *= 1.0 + buffs.bestial_wrath->check_value();

    return m;
  }

  void init_action_list() override
  {
    if ( action_list_str.empty() )
    {
      // Autocast: the longest cooldown first, and an ability short of focus passes to the next one.
      action_list_str = "auto_attack";
      if ( family.bite )
        action_list_str += "/bite";
      if ( family.claw )
        action_list_str += "/claw";
    }
    pet_t::init_action_list();
  }

  action_t* create_action( std::string_view name, std::string_view options_str ) override;
};

struct hunter_pet_attack_t : public melee_attack_t
{
  hunter_pet_attack_t( std::string_view n, hunter_pet_t* p, const spell_data_t* s = spell_data_t::nil() ) :
    melee_attack_t( n, p, s )
  {
    weapon = &p->equipped_weapons[ SLOT_MAIN_HAND ];
    special = true;
  }

  hunter_pet_t* p() const
  { return debug_cast<hunter_pet_t*>( player ); }

  void impact( action_state_t* s ) override
  {
    melee_attack_t::impact( s );

    // Frenzy: a chance on a crit to speed the pet's attacks up.
    if ( s->result == RESULT_CRIT && p()->o()->talent.frenzy.ok() &&
         rng().roll( p()->o()->talent.frenzy->effectN( 1 ).percent() ) )
      p()->buffs.frenzy->trigger();
  }
};

struct pet_melee_t final : public hunter_pet_attack_t
{
  pet_melee_t( hunter_pet_t* p ) : hunter_pet_attack_t( "melee", p )
  {
    weapon_multiplier = 1.0;
    base_execute_time = weapon->swing_time;
    school            = SCHOOL_PHYSICAL;
    trigger_gcd       = 0_ms;
    background = repeating = may_glance = may_crit = may_dodge = may_parry = true;
    special = false;
  }
};

struct pet_auto_attack_t final : public hunter_pet_attack_t
{
  pet_auto_attack_t( hunter_pet_t* p, std::string_view options_str ) : hunter_pet_attack_t( "auto_attack", p )
  {
    parse_options( options_str );
    trigger_gcd = 0_ms;
    ignore_false_positive = true;
    p->main_hand_attack = new pet_melee_t( p );
  }

  void execute() override
  { player->main_hand_attack->schedule_execute(); }

  bool ready() override
  { return player->main_hand_attack->execute_event == nullptr; }
};

// Bite (10 s cooldown, 35 focus) and Claw (25 focus): the rank the hunter's level teaches, flat damage.
struct pet_ability_t final : public hunter_pet_attack_t
{
  pet_ability_t( std::string_view n, hunter_pet_t* p, const spell_data_t* s, std::string_view options_str ) :
    hunter_pet_attack_t( n, p, s )
  {
    parse_options( options_str );
    weapon = nullptr;
    weapon_multiplier = 0;
  }
};

action_t* hunter_pet_t::create_action( std::string_view name, std::string_view options_str )
{
  if ( name == "auto_attack" )
    return new pet_auto_attack_t( this, options_str );
  if ( name == "bite" )
    return new pet_ability_t( name, this, o()->rank.bite.find_rank()->spell(), options_str );
  if ( name == "claw" )
    return new pet_ability_t( name, this, o()->rank.claw.find_rank()->spell(), options_str );

  return pet_t::create_action( name, options_str );
}
}  // namespace pets

bool hunter_t::pet_active() const
{ return pet && !pet->is_sleeping(); }

// Survival talents that only do something in melee range: a hunter with any of them fights in melee.
bool hunter_t::melee_spec() const
{
  return talent.savage_strikes.ok() || talent.predators_edge.ok() || talent.counterattack.ok() ||
         talent.strider_kick.ok() || talent.lacerating_strikes.ok();
}

template <class Base>
struct hunter_action_t : public parse_action_effects_t<Base>
{
  private:
    using ab = parse_action_effects_t<Base>;

  public:
    using base_t = hunter_action_t<Base>;

    template <typename... Args>
    hunter_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {
      if ( ab::data().ok() )
        p()->apply_action_effects( this );
    }

    hunter_t* p()
    { return debug_cast<hunter_t*>( this->player ); }

    const hunter_t* p() const
    { return debug_cast<const hunter_t*>( this->player ); }

    // Melee abilities need melee range and shots their minimum range (8 yd).
    bool ready() override
    {
      if ( ab::data().ok() && this->target )
      {
        double distance = this->player->get_player_distance( *this->target );
        if ( ab::data().max_range() > 0 && distance > ab::data().max_range() )
          return false;
        if ( distance < ab::data().min_range() )
          return false;
      }
      return ab::ready();
    }
};

using hunter_melee_t  = hunter_action_t<melee_attack_t>;
using hunter_spell_t  = hunter_action_t<spell_t>;

struct hunter_ranged_t : public hunter_action_t<ranged_attack_t>
{
  using hunter_action_t<ranged_attack_t>::hunter_action_t;

  // Hunter's Mark: ranged attack power against the marked target.
  void snapshot_internal( action_state_t* s, unsigned flags, result_amount_type rt ) override
  {
    hunter_action_t<ranged_attack_t>::snapshot_internal( s, flags, rt );

    if ( ( flags & STATE_AP ) && s->target )
    {
      if ( auto td = p()->find_target_data( s->target ); td && td->debuff.hunters_mark->check() )
        s->attack_power += td->debuff.hunters_mark->check_value();
    }
  }

  void impact( action_state_t* s ) override;
};

// ================================================================================
// Hunter Buffs
// ================================================================================
struct aspect_of_the_hawk_t : public hunter_spell_t
{
  aspect_of_the_hawk_t( hunter_t* player, util::string_view options_str ) :
    hunter_spell_t( "aspect_of_the_hawk", player, player->rank.aspect_of_the_hawk.find_rank()->spell() )
  {
    parse_options( options_str );

    harmful  = false;
    may_miss = false;

    target   = player;
  }

  void execute() override
  {
    hunter_spell_t::execute();

    p()->buff.aspect_of_the_hawk->trigger();
  }
};

// Trueshot Aura (talent): ranged attack power for the party, cast before the fight.
struct trueshot_aura_t : public hunter_spell_t
{
  trueshot_aura_t( hunter_t* player, util::string_view options_str ) :
    hunter_spell_t( "trueshot_aura", player, player->rank.trueshot_aura.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = may_miss = false;
    target = player;
  }

  bool ready() override
  {
    if ( p()->buff.trueshot_aura->check() )
      return false;
    return hunter_spell_t::ready();
  }

  void execute() override
  {
    hunter_spell_t::execute();
    p()->buff.trueshot_aura->trigger();
  }
};

struct hunters_mark_t : public hunter_spell_t
{
  hunters_mark_t( hunter_t* player, util::string_view options_str ) :
    hunter_spell_t( "hunters_mark", player, player->rank.hunters_mark.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = false;
  }

  bool ready() override
  {
    if ( p()->get_target_data( target )->debuff.hunters_mark->check() )
      return false;
    return hunter_spell_t::ready();
  }

  void execute() override
  {
    hunter_spell_t::execute();
    p()->get_target_data( target )->debuff.hunters_mark->trigger();
  }
};

struct rapid_fire_t : public hunter_spell_t
{
  rapid_fire_t( hunter_t* player, util::string_view options_str ) :
    hunter_spell_t( "rapid_fire", player, player->rank.rapid_fire.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = may_miss = false;
    target = player;
  }

  void execute() override
  {
    hunter_spell_t::execute();
    p()->buff.rapid_fire->trigger();
  }
};

// Bestial Wrath (talent): the pet deals 50% more damage for 18 s.
struct bestial_wrath_t : public hunter_spell_t
{
  bestial_wrath_t( hunter_t* player, util::string_view options_str ) :
    hunter_spell_t( "bestial_wrath", player, player->talent.bestial_wrath )
  {
    parse_options( options_str );
    harmful = may_miss = false;
    target = player;
  }

  bool ready() override
  {
    if ( !p()->pet_active() )
      return false;
    return hunter_spell_t::ready();
  }

  void execute() override
  {
    hunter_spell_t::execute();
    p()->pet->buffs.bestial_wrath->trigger();
  }
};

// Call Pet: the pet is out before the fight and stays out (it is assumed not to die).
struct summon_pet_t : public hunter_spell_t
{
  summon_pet_t( hunter_t* player, util::string_view options_str ) :
    hunter_spell_t( "summon_pet", player, player->find_spell( 883 ) )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = callbacks = false;
    target = player;
  }

  bool ready() override
  {
    if ( !p()->pet || p()->pet_active() || p()->level() < static_cast<int>( data().level() ) )
      return false;
    return hunter_spell_t::ready();
  }

  void execute() override
  {
    hunter_spell_t::execute();
    p()->pet->summon();
    p()->invalidate_cache( CACHE_PLAYER_DAMAGE_MULTIPLIER );
  }
};

// ================================================================================
// Hunter Ranged
// ================================================================================

struct hunter_ranged_attack_t : public hunter_ranged_t
{
  bool first = true;

  hunter_ranged_attack_t( hunter_t* player, util::string_view name )
    : hunter_ranged_t( name, player, player->find_spell( 75 ) )
  {
    weapon            = &player->equipped_weapons[ SLOT_RANGED ];
    weapon_multiplier = 1.0;
    base_execute_time = weapon->swing_time;
    school            = SCHOOL_PHYSICAL;
    trigger_gcd       = 0_ms;

    background        = true;
    repeating         = true;
    may_crit          = true;
    special           = false;
  }

  void reset() override
  {
    hunter_ranged_t::reset();
    first = true;
  }

  timespan_t execute_time() const override
  {
    if ( !player->in_combat )
      return 10_ms;
    if ( first )
      return 100_ms;
    return hunter_ranged_t::execute_time();
  }

  void execute() override
  {
    first = false;
    hunter_ranged_t::execute();
  }
};

struct hunter_auto_shot_t : public hunter_ranged_t
{
  hunter_auto_shot_t( hunter_t* player, util::string_view options_str )
    : hunter_ranged_t( "auto_shot", player, player->find_spell( 75 ) )
  {
    parse_options( options_str );

    trigger_gcd           = 0_ms;
    ignore_false_positive = true;

    assert( player->equipped_weapons[ SLOT_RANGED ].type != WEAPON_NONE );

    player->ranged_attack = new hunter_ranged_attack_t( player, "auto_shot" );
  }

  void execute() override
  {
    p()->ranged_attack->schedule_execute();
  }

  bool ready() override
  {
    // TODO: Copied from auto attacks and not correct for hunter.
    // Figure out the auto shot timers around movement. Roughly seemed like you could move until about 0.5s before
    // the next shot to avoid interruption
    if ( p()->is_moving() )
    {
      return false;
    }

    // No Auto Shot inside its minimum range (melee).
    if ( target && p()->get_player_distance( *target ) < data().min_range() )
      return false;

    return p()->ranged_attack->execute_event == nullptr;
  }
};

struct arcane_shot_t : public hunter_ranged_t
{
  arcane_shot_t( hunter_t* player, util::string_view options_str )
    : hunter_ranged_t( "arcane_shot", player, player->rank.arcane_shot.find_rank()->spell() )
  {
    parse_options( options_str );

    // TODO Forever: Verify at ranks higher than 1. Only tested at low levels
    attack_power_mod.direct = 0.10;
  }
};

struct serpent_sting_t : public hunter_ranged_t
{
  serpent_sting_t( hunter_t* player, util::string_view options_str )
    : hunter_ranged_t( "serpent_sting", player, player->rank.serpent_sting.find_rank()->spell() )
  {
    parse_options( options_str );

    // TODO Forever: Verify with ranks higher than 1. Only tested at low levels
    attack_power_mod.tick = 0.03;
  }
};

// A shot with a cast time: Auto Shot does not fire while the hunter casts, so a shot due during the cast is pushed
// to its end.
struct hunter_cast_shot_t : public hunter_ranged_t
{
  hunter_cast_shot_t( util::string_view n, hunter_t* player, const spell_data_t* s )
    : hunter_ranged_t( n, player, s )
  {
    weapon = &player->equipped_weapons[ SLOT_RANGED ];
  }

  void schedule_execute( action_state_t* s ) override
  {
    hunter_ranged_t::schedule_execute( s );
    p()->delay_auto_shot( time_to_execute );
  }

  bool ready() override
  {
    if ( p()->equipped_weapons[ SLOT_RANGED ].type == WEAPON_NONE )
      return false;
    return hunter_ranged_t::ready();
  }
};

// Aimed Shot and Multi-Shot: normalized ranged weapon damage plus the rank's bonus, sharing one cooldown.
struct aimed_shot_t : public hunter_cast_shot_t
{
  aimed_shot_t( hunter_t* player, util::string_view options_str )
    : hunter_cast_shot_t( "aimed_shot", player, player->rank.aimed_shot.find_rank()->spell() )
  {
    parse_options( options_str );
    cooldown = player->aimed_multi_shot;
    cooldown->duration = data().cooldown();
  }
};

struct multi_shot_t : public hunter_cast_shot_t
{
  multi_shot_t( hunter_t* player, util::string_view options_str )
    : hunter_cast_shot_t( "multi_shot", player, player->rank.multi_shot.find_rank()->spell() )
  {
    parse_options( options_str );
    cooldown = player->aimed_multi_shot;
    cooldown->duration = data().cooldown();
    aoe = std::max( 1, as<int>( data().effectN( 1 ).chain_target() ) );

    // In game Multi-Shot has a 0.5 s cast, which the spell data leaves out (wowsims/forever multi_shot.go).
    base_execute_time = 500_ms;
  }
};

// Sniper Shot (Marksmanship talent): 4 s cast, normalized ranged weapon damage plus the rank's bonus.
struct sniper_shot_t : public hunter_cast_shot_t
{
  sniper_shot_t( hunter_t* player, util::string_view options_str )
    : hunter_cast_shot_t( "sniper_shot", player, player->rank.sniper_shot.find_rank()->spell() )
  {
    parse_options( options_str );
  }
};

// ================================================================================
// Hunter Melee
// ================================================================================

struct raptor_strike_t;

struct hunter_melee_attack_t : public hunter_melee_t
{
  hunter_melee_attack_t( util::string_view n, hunter_t* player, weapon_t* w )
    : hunter_melee_t( n, player, spell_data_t::nil() )
  {
    weapon            = w;
    weapon_multiplier = 1.0;
    base_execute_time = w->swing_time;
    school            = SCHOOL_PHYSICAL;
    trigger_gcd       = 0_ms;

    background = repeating = may_glance = may_crit = may_dodge = may_parry = true;
    special = false;
  }

  void execute() override
  {
    // Raptor Strike replaces the next swing.
    if ( p()->buff.raptor_strike_queued->check() )
    {
      p()->buff.raptor_strike_queued->expire();
      action_t* rs = p()->active.raptor_strike;
      if ( rs->cooldown->up() && p()->resource_available( RESOURCE_MANA, rs->cost() ) )
      {
        p()->active.raptor_strike->execute_on_target( target );
        schedule_execute();
        return;
      }
    }

    hunter_melee_t::execute();
  }

  void impact( action_state_t* s ) override;
};

struct hunter_auto_attack_t : public hunter_melee_t
{
  hunter_auto_attack_t( hunter_t* p, util::string_view options_str )
    : hunter_melee_t( "auto_attack", p, spell_data_t::nil() )
  {
    parse_options( options_str );
    trigger_gcd           = 0_ms;
    ignore_false_positive = true;

    assert( p->equipped_weapons[ SLOT_MAIN_HAND ].type != WEAPON_NONE );

    p->main_hand_attack = new hunter_melee_attack_t( "melee", p, &p->equipped_weapons[ SLOT_MAIN_HAND ] );
  }

  void execute() override
  {
    player->main_hand_attack->schedule_execute();
  }

  bool ready() override
  {
    if ( player->is_moving() || !target || player->get_player_distance( *target ) > 5 )
      return false;
    return player->main_hand_attack->execute_event == nullptr;
  }
};

// Raptor Strike: on the next swing (as warrior Heroic Strike), weapon damage plus the rank's bonus.
struct raptor_strike_t : public hunter_melee_t
{
  raptor_strike_t( hunter_t* player )
    : hunter_melee_t( "raptor_strike", player, player->rank.raptor_strike.find_rank()->spell() )
  {
    background = true;
    trigger_gcd = 0_ms;
  }
};

struct raptor_strike_queue_t : public hunter_melee_t
{
  raptor_strike_queue_t( hunter_t* player, util::string_view options_str )
    : hunter_melee_t( "raptor_strike_queue", player, player->rank.raptor_strike.find_rank()->spell() )
  {
    parse_options( options_str );
    trigger_gcd = 0_ms;
    harmful = false;
    ignore_false_positive = true;
    cooldown = player->get_cooldown( "raptor_strike" );
  }

  bool ready() override
  {
    if ( !p()->active.raptor_strike || p()->buff.raptor_strike_queued->check() || !p()->main_hand_attack )
      return false;
    if ( !p()->active.raptor_strike->cooldown->up() ||
         !p()->resource_available( RESOURCE_MANA, p()->active.raptor_strike->cost() ) )
      return false;
    return hunter_melee_t::ready();
  }

  void execute() override
  {
    p()->buff.raptor_strike_queued->trigger();
  }
};

// Mongoose Bite: usable after a dodge (the target does not attack in a damage simulation) or an Expose Prey proc.
// Lacerating Strikes (talent) bleeds the target for a share of the damage.
struct mongoose_bite_t : public hunter_melee_t
{
  mongoose_bite_t( hunter_t* player, util::string_view options_str )
    : hunter_melee_t( "mongoose_bite", player, player->rank.mongoose_bite.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  bool ready() override
  {
    if ( !p()->buff.expose_prey->check() )
      return false;
    return hunter_melee_t::ready();
  }

  void execute() override
  {
    hunter_melee_t::execute();
    p()->buff.expose_prey->expire();
  }

  void impact( action_state_t* s ) override
  {
    hunter_melee_t::impact( s );

    if ( p()->active.lacerating_strikes && result_is_hit( s->result ) )
      residual_action::trigger( p()->active.lacerating_strikes, s->target,
                                s->result_amount * p()->talent.lacerating_strikes->effectN( 1 ).percent() );
  }
};

struct lacerating_strikes_t : public residual_action::residual_periodic_action_t<melee_attack_t>
{
  lacerating_strikes_t( hunter_t* player )
    : residual_periodic_action_t( "lacerating_strikes", player, player->find_spell( 1310536 ) )
  {
    dot_duration   = data().duration();
    base_tick_time = data().effectN( 1 ).period();
    hasted_ticks = tick_may_crit = may_miss = may_dodge = may_parry = may_block = false;
  }

  void init() override
  {
    residual_periodic_action_t::init();
    snapshot_flags = update_flags = 0;
  }
};

// Strider Kick (Survival talent): normalized weapon damage.
struct strider_kick_t : public hunter_melee_t
{
  strider_kick_t( hunter_t* player, util::string_view options_str )
    : hunter_melee_t( "strider_kick", player, player->talent.strider_kick )
  {
    parse_options( options_str );
  }
};

// Expose Prey (Survival talent): melee and ranged attacks against a target with the hunter's Hunter's Mark may
// allow a Mongoose Bite.
static void trigger_expose_prey( hunter_t* p, action_state_t* s )
{
  if ( !p->talent.expose_prey.ok() || !s->action->result_is_hit( s->result ) )
    return;
  auto td = p->find_target_data( s->target );
  if ( td && td->debuff.hunters_mark->check() && p->rng().roll( p->talent.expose_prey->effectN( 1 ).percent() ) )
    p->buff.expose_prey->trigger();
}

void hunter_melee_attack_t::impact( action_state_t* s )
{
  hunter_melee_t::impact( s );
  trigger_expose_prey( p(), s );
}

void hunter_ranged_t::impact( action_state_t* s )
{
  hunter_action_t<ranged_attack_t>::impact( s );
  trigger_expose_prey( p(), s );
}

// ================================================================================
// Hunter
// ================================================================================

// Casting a shot keeps Auto Shot from firing until the cast ends.
void hunter_t::delay_auto_shot( timespan_t cast_time )
{
  if ( cast_time <= 0_ms || !ranged_attack || !ranged_attack->execute_event )
    return;
  if ( ranged_attack->execute_event->remains() < cast_time )
    ranged_attack->execute_event->reschedule( cast_time );
}

void hunter_t::init_spells()
{
  base_t::init_spells();

  parse_all_class_passives();
  parse_all_passive_talents();

  talent.focused_fire       = find_talent_spell( talent_tree::CLASS, "Focused Fire" );
  talent.unleashed_fury     = find_talent_spell( talent_tree::CLASS, "Unleashed Fury" );
  talent.ferocity           = find_talent_spell( talent_tree::CLASS, "Ferocity" );
  talent.bestial_discipline = find_talent_spell( talent_tree::CLASS, "Bestial Discipline" );
  talent.frenzy             = find_talent_spell( talent_tree::CLASS, "Frenzy" );
  talent.bestial_wrath      = find_talent_spell( talent_tree::CLASS, "Bestial Wrath" );
  talent.lone_wolf          = find_talent_spell( talent_tree::CLASS, "Lone Wolf" );
  talent.trueshot_aura      = find_talent_spell( talent_tree::CLASS, "Trueshot Aura" );
  talent.sniper_shot        = find_talent_spell( talent_tree::CLASS, "Sniper Shot" );
  talent.savage_strikes     = find_talent_spell( talent_tree::CLASS, "Savage Strikes" );
  talent.predators_edge     = find_talent_spell( talent_tree::CLASS, "Predator's Edge" );
  talent.counterattack      = find_talent_spell( talent_tree::CLASS, "Counterattack" );
  talent.expose_prey        = find_talent_spell( talent_tree::CLASS, "Expose Prey" );
  talent.strider_kick       = find_talent_spell( talent_tree::CLASS, "Strider Kick" );
  talent.lacerating_strikes = find_talent_spell( talent_tree::CLASS, "Lacerating Strikes" );

  rank.aspect_of_the_hawk = { this,
    spell_container_t().set_spell( 13165 ),
    spell_container_t().set_spell( 14318 ),
    spell_container_t().set_spell( 14319 ),
    spell_container_t().set_spell( 14320 ),
    spell_container_t().set_spell( 14321 ),
    spell_container_t().set_spell( 14322 ),
    spell_container_t().set_spell( 25296 ),
  };
  rank.arcane_shot = { this,
    spell_container_t().set_spell(  3044 ),
    spell_container_t().set_spell( 14281 ),
    spell_container_t().set_spell( 14282 ),
    spell_container_t().set_spell( 14283 ),
    spell_container_t().set_spell( 14284 ),
    spell_container_t().set_spell( 14285 ),
    spell_container_t().set_spell( 14286 ),
    spell_container_t().set_spell( 14287 ),
  };
  rank.serpent_sting = { this,
    spell_container_t().set_spell(  1978 ),
    spell_container_t().set_spell( 13549 ),
    spell_container_t().set_spell( 13550 ),
    spell_container_t().set_spell( 13551 ),
    spell_container_t().set_spell( 13552 ),
    spell_container_t().set_spell( 13553 ),
    spell_container_t().set_spell( 13554 ),
    spell_container_t().set_spell( 13555 ),
    spell_container_t().set_spell( 25295 ),
  };
  rank.hunters_mark = { this,
    spell_container_t().set_spell(  1130 ),
    spell_container_t().set_spell( 14323 ),
    spell_container_t().set_spell( 14324 ),
    spell_container_t().set_spell( 14325 ),
  };
  rank.multi_shot = { this,
    spell_container_t().set_spell(  2643 ),
  };
  rank.aimed_shot = { this,
    spell_container_t().set_spell( 19434 ),
    spell_container_t().set_spell( 20900 ),
    spell_container_t().set_spell( 20901 ),
    spell_container_t().set_spell( 20902 ),
    spell_container_t().set_spell( 20903 ),
    spell_container_t().set_spell( 20904 ),
  };
  rank.rapid_fire = { this,
    spell_container_t().set_spell(  3045 ),
  };
  rank.raptor_strike = { this,
    spell_container_t().set_spell(  2973 ),
    spell_container_t().set_spell( 14260 ),
    spell_container_t().set_spell( 14261 ),
    spell_container_t().set_spell( 14262 ),
    spell_container_t().set_spell( 14263 ),
    spell_container_t().set_spell( 14264 ),
    spell_container_t().set_spell( 14265 ),
    spell_container_t().set_spell( 14266 ),
  };
  rank.mongoose_bite = { this,
    spell_container_t().set_spell(  1495 ),
    spell_container_t().set_spell( 14269 ),
    spell_container_t().set_spell( 14270 ),
    spell_container_t().set_spell( 14271 ),
  };
  rank.trueshot_aura = { this, talent.trueshot_aura,
    spell_container_t().set_spell( 1299346 ),
    spell_container_t().set_spell( 1299348 ),
    spell_container_t().set_spell(   19506 ),
    spell_container_t().set_spell(   20905 ),
    spell_container_t().set_spell(   20906 ),
  };
  rank.sniper_shot = { this, talent.sniper_shot,
    spell_container_t().set_spell( 1310687 ),
    spell_container_t().set_spell( 1310785 ),
    spell_container_t().set_spell( 1310786 ),
  };
  rank.bite = { this,
    spell_container_t().set_spell( 17253 ),
    spell_container_t().set_spell( 17255 ),
    spell_container_t().set_spell( 17256 ),
    spell_container_t().set_spell( 17257 ),
    spell_container_t().set_spell( 17258 ),
    spell_container_t().set_spell( 17259 ),
    spell_container_t().set_spell( 17260 ),
    spell_container_t().set_spell( 17261 ),
  };
  rank.claw = { this,
    spell_container_t().set_spell( 16827 ),
    spell_container_t().set_spell( 16828 ),
    spell_container_t().set_spell( 16829 ),
    spell_container_t().set_spell( 16830 ),
    spell_container_t().set_spell( 16831 ),
    spell_container_t().set_spell( 16832 ),
    spell_container_t().set_spell(  3010 ),
    spell_container_t().set_spell(  3009 ),
  };

  aimed_multi_shot = get_cooldown( "aimed_multi_shot" );

  // The pet: summon_pet=<family> (or none), and by default a cat unless the hunter has Lone Wolf (+20% damage with
  // no pet out). With a pet out anyway, Lone Wolf's damage is dropped. Hunters tame their first pet at level 10.
  if ( util::str_compare_ci( summon_pet_str, "auto" ) )
  {
    if ( !talent.lone_wolf.ok() )
      pet_family = pets::find_pet_family( "cat" );
  }
  else if ( !util::str_compare_ci( summon_pet_str, "none" ) )
  {
    pet_family = pets::find_pet_family( summon_pet_str );
    if ( !pet_family )
      throw std::invalid_argument( fmt::format( "Unknown summon_pet '{}'", summon_pet_str ) );
  }
  if ( level() < static_cast<int>( find_spell( 883 )->level() ) )
    pet_family = nullptr;
  if ( pet_family && talent.lone_wolf.ok() )
    deregister_passive_spell( talent.lone_wolf );

  if ( rank.raptor_strike.find_rank()->spell()->ok() )
    active.raptor_strike = new raptor_strike_t( this );
  if ( talent.lacerating_strikes.ok() )
    active.lacerating_strikes = new lacerating_strikes_t( this );
}

void hunter_t::init_base_stats()
{
  if ( base.distance < 1 )
  {
    // Survival hunters with melee talents fight in melee range; everyone else at Auto Shot range.
    base.distance = melee_spec() ? 5 : find_spell( 75 )->max_range();
  }
  base.attack_power_per_strength = 1.0;
  base.attack_power_per_agility  = 1.0;

  base.ranged_attack_power_per_agility = 2.0;

  base_t::init_base_stats();

  base.attack_crit_chance = 0;
  // TODO Forever: Only checked on low level characters
  base.spell_crit_chance = 0.036;

  base.stats.attack_power        = 2.0 * level() - 20.0;
  base.stats.ranged_attack_power = 2.0 * level() - 20.0;

  // TODO Forever: Dodge, parry chance
}

void hunter_t::init_action_list()
{
  if ( !action_list_str.empty() )
  {
    base_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  hunter_apl::hunter( this );
  use_default_action_list = true;
  base_t::init_action_list();
}

void hunter_t::init_finished()
{
  parse_player_effects();

  base_t::init_finished();
}

void hunter_t::create_options()
{
  base_t::create_options();
  add_option( opt_string( "summon_pet", summon_pet_str ) );
}

void hunter_t::create_pets()
{
  base_t::create_pets();

  if ( pet_family )
    pet = new pets::hunter_pet_t( this, *pet_family );
}

// pet_active: the hunter's pet is out.
std::unique_ptr<expr_t> hunter_t::create_expression( std::string_view name )
{
  if ( util::str_compare_ci( name, "pet_active" ) )
    return make_fn_expr( name, [ this ] { return pet_active(); } );

  return base_t::create_expression( name );
}

// Focused Fire (talent): more damage while the pet is out.
double hunter_t::composite_player_multiplier( school_e school ) const
{
  double m = base_t::composite_player_multiplier( school );
  if ( talent.focused_fire.ok() && pet_active() )
    m *= 1.0 + talent.focused_fire->effectN( 1 ).percent();
  return m;
}

// Rapid Fire: 40% faster ranged and melee attacks.
double hunter_t::composite_melee_auto_attack_speed() const
{
  double s = base_t::composite_melee_auto_attack_speed();
  if ( buff.rapid_fire && buff.rapid_fire->check() )
    s /= 1.0 + buff.rapid_fire->data().effectN( 4 ).percent();
  return s;
}

double hunter_t::composite_ranged_auto_attack_speed() const
{
  double s = base_t::composite_ranged_auto_attack_speed();
  if ( buff.rapid_fire && buff.rapid_fire->check() )
    s /= 1.0 + buff.rapid_fire->data().effectN( 1 ).percent();
  return s;
}

action_t* hunter_t::create_action( util::string_view name, util::string_view options_str )
{
  if ( name == "auto_shot" )          return new hunter_auto_shot_t( this, options_str );
  if ( name == "auto_attack" )        return new hunter_auto_attack_t( this, options_str );
  if ( name == "arcane_shot" )        return new arcane_shot_t( this, options_str );
  if ( name == "serpent_sting" )      return new serpent_sting_t( this, options_str );
  if ( name == "aspect_of_the_hawk" ) return new aspect_of_the_hawk_t( this, options_str );
  if ( name == "hunters_mark" )       return new hunters_mark_t( this, options_str );
  if ( name == "aimed_shot" )         return new aimed_shot_t( this, options_str );
  if ( name == "multi_shot" )         return new multi_shot_t( this, options_str );
  if ( name == "rapid_fire" )         return new rapid_fire_t( this, options_str );
  if ( name == "trueshot_aura" )      return new trueshot_aura_t( this, options_str );
  if ( name == "sniper_shot" )        return new sniper_shot_t( this, options_str );
  if ( name == "raptor_strike" )      return new raptor_strike_queue_t( this, options_str );
  if ( name == "mongoose_bite" )      return new mongoose_bite_t( this, options_str );
  if ( name == "strider_kick" )       return new strider_kick_t( this, options_str );
  if ( name == "bestial_wrath" )      return new bestial_wrath_t( this, options_str );
  if ( name == "summon_pet" )         return new summon_pet_t( this, options_str );

  return base_t::create_action( name, options_str );
}

void hunter_t::create_buffs()
{
  buff.aspect_of_the_hawk = make_buff<stat_buff_t>( this, "aspect_of_the_hawk", rank.aspect_of_the_hawk.find_rank()->spell() );

  auto trueshot = rank.trueshot_aura.find_rank()->spell();
  buff.trueshot_aura = make_buff_fallback<stat_buff_t>( trueshot->ok(), this, "trueshot_aura", trueshot );

  buff.rapid_fire = make_buff( this, "rapid_fire", rank.rapid_fire.find_rank()->spell() )
                        ->set_cooldown( 0_ms )
                        ->add_invalidate( CACHE_AUTO_ATTACK_SPEED )
                        ->set_stack_change_callback( [ this ]( buff_t*, int, int ) {
                          if ( pet_active() )
                          {
                            pet->invalidate_cache( CACHE_AUTO_ATTACK_SPEED );
                            pet->adjust_auto_attack( gcd_haste_type::AUTO_ATTACK_SPEED );
                          }
                        } );

  buff.raptor_strike_queued = make_buff( this, "raptor_strike_queued" );

  buff.expose_prey = make_buff_fallback( talent.expose_prey.ok(), this, "expose_prey", find_spell( 1310726 ) );

  base_t::create_buffs();
}

// HUNTER MODULE INTERFACE ==================================================

struct hunter_module_t: public module_t
{
  hunter_module_t(): module_t( HUNTER ) {}

  player_t* create_player( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) const override
  {
    auto  player = new hunter_t( sim, name, r );
    return player;
  }

  bool valid() const override { return true; }

  void register_hotfixes() const override {}

  void register_actor_initializers( sim_t* ) const override {}
};

} // UNNAMED NAMESPACE

const module_t* module_t::hunter()
{
  static hunter_module_t m;
  return &m;
}
