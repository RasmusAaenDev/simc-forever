// AULA-RETTELSE (simc-worker/patches/warlock.patch i AULA-Guild): Life Tap, wand (Shoot), Bane of Agony,
// Siphon Life, Shadowburn, Conflagrate, Incinerate, Wrack, Improved Shadow Bolt og en rotation der vaelger
// efter talenter. Daemonerne Imp, Voidwalker, Succubus og Felhunter (valgt efter level og talenter, eller
// default_pet=), Demonic Sacrifice/Pact, Soul Link, Master Demonologist, Demonic Knowledge, Unholy Power,
// Improved Imp/Sayaad og Demonic Energies. Se PATCHES.md.
#include "simulationcraft.hpp"
#include "action/parse_effects.hpp"

#include "class_modules/apl/warlock.hpp"

namespace warlock
{
struct warlock_t final : public player_t
{
  warlock_t( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) :
    player_t( sim, WARLOCK, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_MANA; }

  std::string default_potion() const override   { return warlock_apl::potion( this ); }
  std::string default_flask() const override    { return warlock_apl::flask( this ); }
  std::string default_food() const override     { return warlock_apl::food( this ); }

  action_t* create_action( util::string_view name, util::string_view options_str ) override;
  void init_action_list() override;
  void init_base_stats() override;
  void init_spells() override;
  void create_buffs() override;
  void init_gains() override;

  void create_pets() override;
  void create_options() override;
  void reset() override;
  double composite_player_multiplier( school_e ) const override;
  double composite_spell_power( school_e ) const override;

  struct pets_t
  {
    pet_t* imp = nullptr;
    pet_t* voidwalker = nullptr;
    pet_t* succubus = nullptr;
    pet_t* felhunter = nullptr;
  } pets;

  // AULA-RETTELSE: hvilken daemon warlocken har ude, og hvilken den har ofret (Demonic Sacrifice).
  enum demon_e { DEMON_NONE = 0, DEMON_IMP, DEMON_VOIDWALKER, DEMON_SUCCUBUS, DEMON_FELHUNTER };
  std::string default_pet_str = "auto";        // default_pet=auto|none|imp|voidwalker|succubus|felhunter
  std::string demonic_sacrifice_str = "auto";  // demonic_sacrifice=auto|none|imp|succubus
  demon_e chosen_demon    = DEMON_NONE;  // daemonen ude i kampen (valgt i choose_demons)
  demon_e sacrifice_demon = DEMON_NONE;  // daemonen der ofres foer kampen
  demon_e active_demon    = DEMON_NONE;  // daemonen ude lige nu
  demon_e sacrificed      = DEMON_NONE;  // daemonen hvis Demonic Sacrifice-effekt er oppe

  pet_t* demon_pet( demon_e d ) const;
  bool demon_available( demon_e d ) const;
  demon_e next_demon_to_summon() const;
  void choose_demons();
  void summon_demon( demon_e d );
  void sacrifice_active_demon();

  struct
  {
    spell_ranks_t corruption;
    spell_ranks_t firebolt;
    spell_ranks_t immolate;
    spell_ranks_t shadow_bolt;
    spell_ranks_t life_tap;
    spell_ranks_t bane_of_agony;
    spell_ranks_t siphon_life;
    spell_ranks_t shadowburn;
    spell_ranks_t conflagrate;
    spell_ranks_t incinerate;
    spell_ranks_t wrack;
    spell_ranks_t lash_of_pain;
  } rank;

  struct
  {
    player_talent_t improved_life_tap;
    player_talent_t siphon_life;
    player_talent_t shadowburn;
    player_talent_t conflagrate;
    player_talent_t incinerate;
    player_talent_t wrack;
    player_talent_t improved_shadow_bolt;
    // Demonology (daemonerne)
    player_talent_t demonic_sacrifice;
    player_talent_t demonic_pact;
    player_talent_t soul_link;
    player_talent_t master_demonologist;
    player_talent_t demonic_knowledge;
    player_talent_t unholy_power;
    player_talent_t improved_imp;
    player_talent_t improved_sayaad;
    player_talent_t demonic_energies;
  } talent;

  const spell_data_t* soul_link_effect = nullptr;  // 25228: +3 % skade for warlock og daemon

  struct
  {
    // Improved Shadow Bolt: Shadow Vulnerability (17794). Ligger paa warlocken (kun dens egen skade
    // paavirkes, og Patchwerk har eet maal); med flere maal gaelder den dermed for alle.
    buff_t* shadow_vulnerability = nullptr;
    // Demonic Sacrifice: ofret Imp = Burning Shadow (18789, +15 % Shadow), ofret Succubus = Touch of Fire
    // (18791, +15 % Fire). Omvendt af original Classic; spildataen er facit.
    buff_t* burning_shadow = nullptr;
    buff_t* touch_of_fire  = nullptr;
  } buff;

  struct
  {
    gain_t* life_tap = nullptr;
    gain_t* life_tap_demon = nullptr;
  } gains;

  // Til Conflagrate (forbruger Immolate) og Wrack (forstaerker de andre Shadow-DoTs).
  action_t* immolate_action = nullptr;
  action_t* wrack_action    = nullptr;
};

struct warlock_module_t : public module_t
{
  warlock_module_t() : module_t( WARLOCK )
  { }

  player_t* create_player( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) const override
  {
    auto p = new warlock_t( sim, name, r );
    return p;
  }

  bool valid() const override
  { return true; }

  void register_hotfixes() const override {}

  void register_actor_initializers( sim_t* ) const override {}
};

template <class Base>
struct warlock_action_t : public parse_action_effects_t<Base>
{
private:
  using ab = parse_action_effects_t<Base>;

public:
  using base_t = warlock_action_t<Base>;

  template <typename... Args>
  warlock_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {}

  warlock_t* p()
  { return debug_cast<warlock_t*>( this->player ); }

  const warlock_t* p() const
  { return debug_cast<const warlock_t*>( this->player ); }

  // Maalets debuffs fra warlocken: Shadow Vulnerability (Improved Shadow Bolt) paa al Shadow-skade, og
  // Wrack (+10 % paa warlockens andre Shadow-DoTs, mens den kanaliseres). Maal-multiplikatoren
  // genberegnes ved hvert tick, saa begge virker ogsaa paa DoTs, der allerede tikker.
  double composite_target_multiplier( player_t* t ) const override
  {
    double m = ab::composite_target_multiplier( t );
    if ( this->get_school() == SCHOOL_SHADOW && p()->buff.shadow_vulnerability &&
         p()->buff.shadow_vulnerability->check() )
      m *= 1.0 + p()->buff.shadow_vulnerability->check_value();
    return m;
  }

  double composite_target_ta_multiplier( player_t* t ) const override
  {
    double m = ab::composite_target_ta_multiplier( t );
    const action_t* wrack = p()->wrack_action;
    if ( wrack && wrack != this && this->get_school() == SCHOOL_SHADOW )
    {
      const dot_t* d = wrack->find_dot( t );
      if ( d && d->is_ticking() )
        m *= 1.0 + wrack->data().effectN( 2 ).percent();
    }
    return m;
  }
};

using warlock_spell_t  = warlock_action_t<spell_t>;

// Shadow Bolt
struct shadow_bolt_t : public warlock_spell_t
{
  shadow_bolt_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "shadow_bolt", player, player->rank.shadow_bolt.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  // Improved Shadow Bolt: et crit giver Shadow Vulnerability (+4-20 % Shadow-skade efter rank, 12 s).
  void impact( action_state_t* s ) override
  {
    warlock_spell_t::impact( s );
    if ( p()->talent.improved_shadow_bolt.ok() && s->result == RESULT_CRIT )
      p()->buff.shadow_vulnerability->trigger( 1, p()->talent.improved_shadow_bolt->effectN( 1 ).percent() );
  }
};

// Immolate
struct immolate_t : public warlock_spell_t
{
  immolate_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "immolate", player, player->rank.immolate.find_rank()->spell() )
  {
    parse_options( options_str );
    if ( !player->immolate_action )
      player->immolate_action = this;
  }
};

// Corruption
struct corruption_t : public warlock_spell_t
{
  corruption_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "corruption", player, player->rank.corruption.find_rank()->spell() )
  {
    parse_options( options_str );
  }
};

// Shoot (wand, 5019): samme model som mage- og priest-rettelsen. Wandens vaabenskade fra
// ranged-pladsen, spell-tabellen (hit/crit), ingen rustning, wandens hastighed, ingen mana.
// Uden wand i ranged-pladsen er handlingen aldrig klar.
struct shoot_t : public warlock_spell_t
{
  shoot_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "shoot", player, player->find_spell( 5019 ) )
  {
    parse_options( options_str );

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

  double composite_target_armor( const action_state_t* ) const override
  { return 0.0; }

  timespan_t execute_time() const override
  { return base_execute_time; }

  bool ready() override
  {
    if ( !has_wand() )
      return false;
    return warlock_spell_t::ready();
  }
};

// Life Tap (1454 ...): (effekt 1 + Spirit) liv bliver til lige saa meget mana, Improved Life Tap
// (18182) oeger kun manaen (som wowsims/forever lifetap.go). Spillerens liv er uendeligt i simc
// (ingen indgaaende skade i en DPS-simulering), saa livet traekkes ikke; antagelse: en healer
// eller pauser daekker det. Ingen mana-pris, saa femsekundsreglen nulstilles ikke.
struct life_tap_t : public warlock_spell_t
{
  life_tap_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "life_tap", player, player->rank.life_tap.find_rank()->spell() )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = callbacks = false;
    target = player;
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct = 0;
  }

  double mana_gain() const
  {
    double amount = data().effectN( 1 ).average( p() ) + p()->cache.spirit();
    if ( p()->talent.improved_life_tap.ok() )
      amount *= 1.0 + p()->talent.improved_life_tap->effectN( 1 ).percent();
    return amount;
  }

  void execute() override
  {
    warlock_spell_t::execute();
    double gain = mana_gain();
    p()->resource_gain( RESOURCE_MANA, gain, p()->gains.life_tap, this );
    // Demonic Energies (1225214, effekt 2): daemonen ude faar 50/100 % af manaen (wowsims/forever lifetap.go).
    if ( p()->talent.demonic_energies.ok() )
      if ( pet_t* d = p()->demon_pet( p()->active_demon ) )
        d->resource_gain( RESOURCE_MANA, gain * p()->talent.demonic_energies->effectN( 2 ).percent(),
                          p()->gains.life_tap_demon, this );
  }
};

// Bane of Agony (980 ...): Forever-navnet for Curse of Agony (instant, 24 s). Skaden bygger op: et
// tick i foerste tredjedel giver det halve af grundvaerdien, i anden hele, i sidste halvanden gang
// (wowsims/forever agony.go). Spell power-andelen stiger ikke. Summen er den samme som et jaevnt tick.
struct bane_of_agony_t : public warlock_spell_t
{
  bane_of_agony_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "bane_of_agony", player, player->rank.bane_of_agony.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  double base_ta( const action_state_t* s ) const override
  {
    double b = warlock_spell_t::base_ta( s );
    const dot_t* d = find_dot( s->target );
    if ( !d || !d->is_ticking() )
      return b;
    int n = d->num_ticks();
    int k = d->current_tick;
    if ( n <= 0 || k <= 0 )
      return b;
    if ( 3 * k <= n )
      return b * 0.5;
    if ( 3 * k <= 2 * n )
      return b;
    return b * 1.5;
  }
};

// Siphon Life (talent, 18265 ...): Periodic Health Leech; kun skaden simuleres (livet er uendeligt).
struct siphon_life_t : public warlock_spell_t
{
  siphon_life_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "siphon_life", player, player->rank.siphon_life.find_rank()->spell() )
  {
    parse_options( options_str );
  }
};

// Shadowburn (talent, 17877 ...): instant Shadow-skade, 15 s nedkoeling (effekt 2; effekt 1 er
// soul shard-auraen ved maalets doed og simuleres ikke).
struct shadowburn_t : public warlock_spell_t
{
  shadowburn_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "shadowburn", player, player->rank.shadowburn.find_rank()->spell() )
  {
    parse_options( options_str );
    dot_duration = 0_ms;
  }
};

// Conflagrate (talent, 1293817 ...): kraever warlockens Immolate paa maalet og fjerner den.
struct conflagrate_t : public warlock_spell_t
{
  conflagrate_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "conflagrate", player, player->rank.conflagrate.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  dot_t* immolate_dot( player_t* t ) const
  { return p()->immolate_action ? p()->immolate_action->find_dot( t ) : nullptr; }

  bool target_ready( player_t* t ) override
  {
    const dot_t* d = immolate_dot( t );
    if ( !d || !d->is_ticking() )
      return false;
    return warlock_spell_t::target_ready( t );
  }

  void impact( action_state_t* s ) override
  {
    warlock_spell_t::impact( s );
    if ( result_is_hit( s->result ) )
      if ( dot_t* d = immolate_dot( s->target ) )
        d->cancel();
  }
};

// Incinerate (talent, 412758 ...): Fire-skade, +25 % (effekt 2) hvis maalet har warlockens Immolate.
struct incinerate_t : public warlock_spell_t
{
  incinerate_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "incinerate", player, player->rank.incinerate.find_rank()->spell() )
  {
    parse_options( options_str );
  }

  double composite_target_da_multiplier( player_t* t ) const override
  {
    double m = warlock_spell_t::composite_target_da_multiplier( t );
    const dot_t* d = p()->immolate_action ? p()->immolate_action->find_dot( t ) : nullptr;
    if ( d && d->is_ticking() )
      m *= 1.0 + data().effectN( 2 ).percent();
    return m;
  }
};

// Wrack (talent, 1316697): kanaliseret 6 s, Shadow-skade hvert sekund; mens den tikker, tager maalet
// +10 % fra warlockens andre Shadow-DoTs (se composite_target_ta_multiplier ovenfor).
struct wrack_t : public warlock_spell_t
{
  wrack_t( warlock_t* player, util::string_view options_str ) :
    warlock_spell_t( "wrack", player, player->rank.wrack.find_rank()->spell() )
  {
    parse_options( options_str );
    channeled = true;
    if ( !player->wrack_action )
      player->wrack_action = this;
  }
};

// AULA-RETTELSE: Summon Imp (688, level 1), Voidwalker (697, 10), Succubus (712, 20) og Felhunter (691, 30).
// summon_demon tilkalder den daemon, warlocken skal have ude (choose_demons), eller foerst den, der skal ofres.
// Foer kampen (precombat) koster en tilkaldelse ingen mana: antagelse, at warlocken tilkalder og drikker foer
// pull. I kampen gaelder spildataens pris og 10 s cast (Fel Domination modelleres ikke).
struct summon_demon_t final : public warlock_spell_t
{
  warlock_t::demon_e which;  // DEMON_NONE = automatisk valg

  summon_demon_t( util::string_view n, warlock_t* player, const spell_data_t* s, warlock_t::demon_e d,
                  std::string_view options_str ) :
    warlock_spell_t( n, player, s ), which( d )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = callbacks = false;
    target = player;
  }

  warlock_t::demon_e demon() const
  { return which != warlock_t::DEMON_NONE ? which : p()->next_demon_to_summon(); }

  double cost() const override
  { return player->in_combat ? warlock_spell_t::cost() : 0.0; }

  bool ready() override
  {
    auto d = demon();
    if ( d == warlock_t::DEMON_NONE || d == p()->active_demon || !p()->demon_available( d ) )
      return false;
    return warlock_spell_t::ready();
  }

  void execute() override
  {
    auto d = demon();
    warlock_spell_t::execute();
    p()->summon_demon( d );
  }
};

// Demonic Sacrifice (18788, talent): ofrer daemonen ude og giver warlocken dens effekt (7200 s, dvs. hele
// kampen), indtil en daemon tilkaldes; med Demonic Pact (425464) kun hvis det er den ofrede daemon igen.
struct demonic_sacrifice_t final : public warlock_spell_t
{
  demonic_sacrifice_t( warlock_t* player, std::string_view options_str ) :
    warlock_spell_t( "demonic_sacrifice", player, player->talent.demonic_sacrifice )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = callbacks = false;
    target = player;
  }

  bool ready() override
  {
    if ( !p()->talent.demonic_sacrifice.ok() || p()->active_demon == warlock_t::DEMON_NONE ||
         p()->active_demon != p()->sacrifice_demon || p()->sacrificed != warlock_t::DEMON_NONE )
      return false;
    return warlock_spell_t::ready();
  }

  void execute() override
  {
    warlock_spell_t::execute();
    p()->sacrifice_active_demon();
  }
};

///////////////////////////////////////////
//
// Demons
//
///////////////////////////////////////////

namespace pets {
  // AULA-RETTELSE: faelles daemon-model efter wowsims/forever sim/warlock/pets.go (MIT; formlerne er skrevet som
  // andele af creature-vaerdierne, saa de gaelder paa alle level). Fra warlocken: AP = 0,17 x spell damage
  // (upstream), spell damage = 0,10 x warlockens, warlockens spell crit som daemonens crit (upstream for Imp'en),
  // spell hit = warlockens, melee hit = 8/17 af den. Mana-regen er en andel af warlockens Intellect som MP5
  // (Imp 0,30, de andre 0,10; maalt paa level 30). Upstreams Imp-regen (1,65/2,73 mana/s) var kun maalt paa level 7.
  struct warlock_pet_t : public pet_t
  {
    warlock_t::demon_e demon;
    double damage_mod         = 1.0;   // Voidwalker 0,5, Felhunter 0,8 (wowsims damageMod)
    double mp5_per_owner_int  = 0.10;

    warlock_pet_t( sim_t* sim, warlock_t* owner, std::string_view pet_name, pet_e pet_type, warlock_t::demon_e d ) :
      pet_t( sim, owner, pet_name, pet_type, false, false ), demon( d )
    {
      // from level 6-10 imp character sheet (upstream)
      owner_coeff.armor = 0.35;
      owner_coeff.ap_from_sp = 0.17;
      owner_coeff.sp_from_sp = 0.10;

      // TODO: Imp HP looks to come from a table
      owner_coeff.health = 1.5;
      stamina_per_owner = 0;

      // matches in-game stats but int might not actually do anything on the pet
      intellect_per_owner = 0.3;
    }

    const warlock_t* o() const
    { return static_cast<warlock_t*>( owner ); }

    warlock_t* o()
    { return static_cast<warlock_t*>( owner ); }

    resource_e primary_resource() const override { return RESOURCE_MANA; }

    // Daemonens manapulje: upstreams Imp-fit (29 + 9,5 x level, maalt paa level 3-8) op til level 8, derefter
    // lineaert til wowsims' level 60-pulje (576 + 5 x 264 Int = 1896). SKOEN: puljen mellem level 9 og 59 er ikke
    // maalt, og i spillet viser pet-arket paa level 60 ca. 10.870 mana mere (fra warlocken; wowsims' TODO).
    void init_base_stats() override
    {
      pet_t::init_base_stats();
      double l = level();
      double mana = l <= 8 ? 29 + 9.5 * l : 105 + ( 1896.0 - 105.0 ) * ( std::min( l, 60.0 ) - 8 ) / 52.0;
      resources.base[ RESOURCE_MANA ] = std::floor( mana );
    }

    double resource_regen_per_second( resource_e r ) const override
    {
      if ( r != RESOURCE_MANA )
        return pet_t::resource_regen_per_second( r );
      return owner->cache.intellect() * mp5_per_owner_int / 5.0;
    }

    // Warlockens spell crit (melee og spell), som Imp'en i upstream og wowsims.
    double pet_crit() const override
    { return current_pet_stats.composite_spell_crit; }

    double composite_spell_hit() const override
    { return owner->cache.spell_hit(); }

    double composite_melee_hit() const override
    { return owner->cache.spell_hit() * 8.0 / 17.0; }

    double composite_melee_expertise( const weapon_t* ) const override
    { return 0.0; }

    // Demonic Knowledge (412732): + rankens andel af warlockens level i spell damage, mens en daemon er ude.
    double composite_spell_power( school_e school ) const override
    {
      double sp = pet_t::composite_spell_power( school );
      if ( o()->talent.demonic_knowledge.ok() )
        sp += o()->level() * o()->talent.demonic_knowledge->effectN( 1 ).percent();
      return sp;
    }

    // Daemonens egne multiplikatorer: Unholy Power (18769, +2-10 %), Soul Link (25228, +3 %), Master
    // Demonologist (23785: Imp +Fire, Succubus +Shadow; Voidwalker/Felhunter er forsvar) og daemonens
    // basis-modifikator.
    double composite_player_multiplier( school_e school ) const override
    {
      double m = pet_t::composite_player_multiplier( school );
      m *= damage_mod;
      if ( o()->talent.unholy_power.ok() )
        m *= 1.0 + o()->talent.unholy_power->effectN( 1 ).percent();
      if ( o()->talent.soul_link.ok() && o()->soul_link_effect )
        m *= 1.0 + o()->soul_link_effect->effectN( 1 ).percent();
      if ( o()->talent.master_demonologist.ok() )
      {
        if ( demon == warlock_t::DEMON_IMP && school == SCHOOL_FIRE )
          m *= 1.0 + o()->talent.master_demonologist->effectN( 1 ).percent();
        if ( demon == warlock_t::DEMON_SUCCUBUS && school == SCHOOL_SHADOW )
          m *= 1.0 + o()->talent.master_demonologist->effectN( 3 ).percent();
      }
      return m;
    }
  };

  template <class Base>
  struct warlock_pet_action_t : parse_action_effects_t<Base>
  {
  private:
    using ab = parse_action_effects_t<Base>;
  public:
    using base_t = warlock_pet_action_t<Base>;

    template <typename... Args>
    warlock_pet_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {}

    warlock_pet_t* p()
    { return debug_cast<warlock_pet_t*>( this->player ); }

    const warlock_pet_t* p() const
    { return debug_cast<const warlock_pet_t*>( this->player ); }

    warlock_t* o()
    { return debug_cast<warlock_t*>( p()->owner ); }

    const warlock_t* o() const
    { return debug_cast<const warlock_t*>( p()->owner ); }
  };

  using warlock_pet_attack_t = warlock_pet_action_t<melee_attack_t>;
  using warlock_pet_spell_t  = warlock_pet_action_t<spell_t>;

  // Naerkamp for Voidwalker, Succubus og Felhunter: vaabnet er en andel af creature_auto_attack_dps for
  // daemonens level (expected_stat.inc) x 2,0 s, +/- spredning; AP = 3 x level + 6 (wowsims: linje gennem
  // Succubus' level 20- og 30-ark) plus 0,17 x warlockens spell damage.
  struct melee_demon_t : public warlock_pet_t
  {
    double swing_share;   // gennemsnitligt slag = swing_share x creature dps x 2,0 s
    double swing_spread;  // +/- andel

    melee_demon_t( sim_t* sim, warlock_t* owner, std::string_view n, pet_e t, warlock_t::demon_e d, double share,
                   double spread ) :
      warlock_pet_t( sim, owner, n, t, d ), swing_share( share ), swing_spread( spread )
    {}

    void init_base_stats() override
    {
      warlock_pet_t::init_base_stats();
      double avg = swing_share * dbc->expected_stat( level() ).creature_auto_attack_dps * 2.0;
      equipped_weapons[ SLOT_MAIN_HAND ].type       = WEAPON_BEAST;
      equipped_weapons[ SLOT_MAIN_HAND ].swing_time = 2_s;
      equipped_weapons[ SLOT_MAIN_HAND ].min_dmg    = avg * ( 1.0 - swing_spread );
      equipped_weapons[ SLOT_MAIN_HAND ].max_dmg    = avg * ( 1.0 + swing_spread );
      equipped_weapons[ SLOT_MAIN_HAND ].damage     = ( equipped_weapons[ SLOT_MAIN_HAND ].min_dmg + equipped_weapons[ SLOT_MAIN_HAND ].max_dmg ) / 2.0;
    }

    double composite_melee_attack_power() const override
    { return warlock_pet_t::composite_melee_attack_power() + 3.0 * level() + 6.0; }

    void init_action_list() override
    {
      if ( action_list_str.empty() )
        action_list_str = demon == warlock_t::DEMON_SUCCUBUS ? "auto_attack/lash_of_pain" : "auto_attack";
      warlock_pet_t::init_action_list();
    }

    action_t* create_action( std::string_view name, std::string_view options_str ) override;
  };

  struct demon_melee_t : public warlock_pet_attack_t
  {
    demon_melee_t( melee_demon_t* p ) : warlock_pet_attack_t( "melee", p, spell_data_t::nil() )
    {
      weapon            = &p->equipped_weapons[ SLOT_MAIN_HAND ];
      weapon_multiplier = 1.0;
      base_execute_time = weapon->swing_time;
      school            = SCHOOL_PHYSICAL;
      trigger_gcd       = 0_ms;
      background = repeating = may_glance = may_crit = may_dodge = may_parry = true;
      special = false;
    }
  };

  struct demon_auto_attack_t : public warlock_pet_attack_t
  {
    demon_auto_attack_t( melee_demon_t* p, std::string_view options_str ) :
      warlock_pet_attack_t( "auto_attack", p, spell_data_t::nil() )
    {
      parse_options( options_str );
      trigger_gcd = 0_ms;
      ignore_false_positive = true;
      p->main_hand_attack = new demon_melee_t( p );
    }

    void execute() override
    { player->main_hand_attack->schedule_execute(); }

    bool ready() override
    { return player->main_hand_attack->execute_event == nullptr; }
  };

  // Lash of Pain (7814 ... 11780; 20/28/36/44/52/60): Shadow, 12 s nedkoeling, 0,429 x daemonens spell damage.
  // Improved Sayaad (18754, effekt 1) oeger skaden.
  struct lash_of_pain_t : public warlock_pet_spell_t
  {
    lash_of_pain_t( melee_demon_t* p, std::string_view options_str ) :
      warlock_pet_spell_t( "lash_of_pain", p, p->o()->rank.lash_of_pain.find_rank()->spell() )
    {
      parse_options( options_str );
    }

    double composite_da_multiplier( const action_state_t* s ) const override
    {
      double m = warlock_pet_spell_t::composite_da_multiplier( s );
      if ( o()->talent.improved_sayaad.ok() )
        m *= 1.0 + o()->talent.improved_sayaad->effectN( 1 ).percent();
      return m;
    }
  };

  action_t* melee_demon_t::create_action( std::string_view name, std::string_view options_str )
  {
    if ( name == "auto_attack" ) return new demon_auto_attack_t( this, options_str );
    if ( name == "lash_of_pain" ) return new lash_of_pain_t( this, options_str );
    return warlock_pet_t::create_action( name, options_str );
  }

  namespace imp {
    struct imp_pet_t : warlock_pet_t
    {
      imp_pet_t( sim_t* sim, warlock_t* owner ) :
        warlock_pet_t( sim, owner, "imp", PET_IMP, warlock_t::DEMON_IMP )
      {
        mp5_per_owner_int = 0.30;
      }

      void init_action_list() override
      {
        action_list_str = "firebolt";
        warlock_pet_t::init_action_list();
      }

      action_t* create_action( std::string_view, std::string_view ) override;
    };

    // TODO: There's some funky delay on beta. Some casts are back-to-back, some have a 0.5-1.0s gap even when
    // the imp has mana
    struct firebolt_t : warlock_pet_spell_t
    {
      firebolt_t( std::string_view n, imp_pet_t* p, std::string_view options_str  ) :
        warlock_pet_spell_t( n, p, p->o()->rank.firebolt.find_rank()->spell() )
      {
        parse_options( options_str );

        // beta casts have been wonky with gaps in between casts
        //ability_lag.mean   = 500_ms;
        //ability_lag.stddev = 100_ms;
      }

      // Improved Imp (18694, effekt 2): +10/20/30 % Firebolt-skade.
      double composite_da_multiplier( const action_state_t* s ) const override
      {
        double m = warlock_pet_spell_t::composite_da_multiplier( s );
        if ( o()->talent.improved_imp.ok() )
          m *= 1.0 + o()->talent.improved_imp->effectN( 2 ).percent();
        return m;
      }
    };

    action_t* imp_pet_t::create_action( std::string_view name, std::string_view options_str )
    {
      if ( name == "firebolt" ) return new firebolt_t( name, this, options_str );

      return warlock_pet_t::create_action( name, options_str );
    }
  }
}

pet_t* warlock_t::demon_pet( demon_e d ) const
{
  switch ( d )
  {
    case DEMON_IMP:        return pets.imp;
    case DEMON_VOIDWALKER: return pets.voidwalker;
    case DEMON_SUCCUBUS:   return pets.succubus;
    case DEMON_FELHUNTER:  return pets.felhunter;
    default:               return nullptr;
  }
}

// Kan warlocken tilkalde daemonen paa sit level? (Summon-spellens level i spildataen: 1/10/20/30.)
bool warlock_t::demon_available( demon_e d ) const
{
  static const unsigned summon_id[] = { 0, 688, 697, 712, 691 };
  if ( d == DEMON_NONE )
    return false;
  const spell_data_t* s = find_spell( summon_id[ d ] );
  return s->ok() && level() >= static_cast<int>( s->level() );
}

warlock_t::demon_e warlock_t::next_demon_to_summon() const
{
  if ( sacrifice_demon != DEMON_NONE && sacrificed == DEMON_NONE )
    return sacrifice_demon;
  return chosen_demon;
}

// Daemonvalget (AULA-RETTELSE). Profilen kan selv angive default_pet= og demonic_sacrifice=; ellers (auto):
// - Daemonen ude: Succubus fra level 20 (den daemon, der goer mest skade, og som specs bruger, naar det gaelder
//   skade), Voidwalker paa level 10-19 (leveling-guidernes valg), Imp under level 10. Felhunter vaelges aldrig
//   automatisk (dens evner er utility), men kan vaelges med default_pet=felhunter.
// - Demonic Sacrifice: kun med Demonic Pact. Saa ofres Imp'en foer kampen (Burning Shadow, +15 % Shadow), og
//   daemonen ovenfor tilkaldes bagefter (Icy Veins' Demonology-build). Uden Pact beholdes daemonen: maalt i
//   simc giver Succubus/Voidwalker mere end +15 % Shadow (se PATCHES.md).
void warlock_t::choose_demons()
{
  auto parse = []( std::string v, demon_e& out ) {
    util::tolower( v );
    if ( v == "none" ) { out = DEMON_NONE; return true; }
    if ( v == "imp" ) { out = DEMON_IMP; return true; }
    if ( v == "voidwalker" ) { out = DEMON_VOIDWALKER; return true; }
    if ( v == "succubus" || v == "sayaad" ) { out = DEMON_SUCCUBUS; return true; }
    if ( v == "felhunter" ) { out = DEMON_FELHUNTER; return true; }
    return false;
  };

  demon_e best = demon_available( DEMON_SUCCUBUS )     ? DEMON_SUCCUBUS
                 : demon_available( DEMON_VOIDWALKER ) ? DEMON_VOIDWALKER
                                                       : DEMON_IMP;
  chosen_demon    = best;
  sacrifice_demon = DEMON_NONE;

  if ( !util::str_compare_ci( default_pet_str, "auto" ) )
  {
    demon_e d = DEMON_NONE;
    if ( !parse( default_pet_str, d ) )
      throw std::invalid_argument( fmt::format( "Unknown default_pet '{}' (auto, none, imp, voidwalker, succubus, felhunter)",
                                                default_pet_str ) );
    chosen_demon = d == DEMON_NONE || demon_available( d ) ? d : best;
  }

  if ( util::str_compare_ci( demonic_sacrifice_str, "auto" ) )
  {
    if ( talent.demonic_sacrifice.ok() && talent.demonic_pact.ok() && chosen_demon != DEMON_NONE &&
         chosen_demon != DEMON_IMP )
      sacrifice_demon = DEMON_IMP;
  }
  else
  {
    demon_e d = DEMON_NONE;
    if ( !parse( demonic_sacrifice_str, d ) || d == DEMON_VOIDWALKER || d == DEMON_FELHUNTER )
      throw std::invalid_argument( fmt::format( "Unknown demonic_sacrifice '{}' (auto, none, imp, succubus)",
                                                demonic_sacrifice_str ) );
    sacrifice_demon = talent.demonic_sacrifice.ok() && demon_available( d ) ? d : DEMON_NONE;
    // Uden Demonic Pact ophaever en ny daemon effekten: ingen daemon ude.
    if ( sacrifice_demon != DEMON_NONE && !talent.demonic_pact.ok() )
      chosen_demon = DEMON_NONE;
  }

  // Den ofrede daemon kan ikke ogsaa vaere ude (en ny tilkaldelse af den ophaever effekten): med Demonic Pact
  // tilkaldes i stedet Imp'en (ofret Succubus) eller den bedste anden daemon. (Kun ved demonic_sacrifice=.)
  if ( sacrifice_demon != DEMON_NONE && chosen_demon == sacrifice_demon )
    chosen_demon = sacrifice_demon == DEMON_SUCCUBUS ? DEMON_IMP : best != sacrifice_demon ? best : DEMON_NONE;
}

void warlock_t::summon_demon( demon_e d )
{
  if ( pet_t* cur = demon_pet( active_demon ) )
    cur->dismiss();
  active_demon = DEMON_NONE;

  // En tilkaldelse ophaever Demonic Sacrifice; med Demonic Pact kun naar det er den ofrede daemon.
  if ( sacrificed != DEMON_NONE && ( !talent.demonic_pact.ok() || d == sacrificed ) )
  {
    buff.burning_shadow->expire();
    buff.touch_of_fire->expire();
    sacrificed = DEMON_NONE;
  }

  if ( pet_t* pet = demon_pet( d ) )
  {
    pet->summon();
    active_demon = d;
  }
  invalidate_cache( CACHE_PLAYER_DAMAGE_MULTIPLIER );
  invalidate_cache( CACHE_SPELL_POWER );
}

void warlock_t::sacrifice_active_demon()
{
  demon_e d = active_demon;
  if ( pet_t* cur = demon_pet( d ) )
    cur->dismiss();
  active_demon = DEMON_NONE;

  buff.burning_shadow->expire();
  buff.touch_of_fire->expire();
  if ( d == DEMON_IMP )
    buff.burning_shadow->trigger();
  else if ( d == DEMON_SUCCUBUS )
    buff.touch_of_fire->trigger();
  sacrificed = d;
  invalidate_cache( CACHE_PLAYER_DAMAGE_MULTIPLIER );
  invalidate_cache( CACHE_SPELL_POWER );
}

void warlock_t::create_options()
{
  player_t::create_options();
  add_option( opt_string( "default_pet", default_pet_str ) );
  add_option( opt_string( "demonic_sacrifice", demonic_sacrifice_str ) );
}

void warlock_t::reset()
{
  player_t::reset();
  active_demon = DEMON_NONE;
  sacrificed   = DEMON_NONE;
}

// Warlockens egne multiplikatorer fra daemonerne: Demonic Sacrifice (effekt 1 paa sin skole), og mens en
// daemon er ude Soul Link (+3 %) og Master Demonologist (Imp: Fire, Succubus: Shadow; effekt 1 hhv. 3).
double warlock_t::composite_player_multiplier( school_e school ) const
{
  double m = player_t::composite_player_multiplier( school );
  for ( const buff_t* b : { buff.burning_shadow, buff.touch_of_fire } )
    if ( b && b->check() && b->data().effectN( 1 ).has_common_school( school ) )
      m *= 1.0 + b->data().effectN( 1 ).percent();
  if ( active_demon != DEMON_NONE )
  {
    if ( talent.soul_link.ok() && soul_link_effect )
      m *= 1.0 + soul_link_effect->effectN( 1 ).percent();
    if ( talent.master_demonologist.ok() )
    {
      if ( active_demon == DEMON_IMP && school == SCHOOL_FIRE )
        m *= 1.0 + talent.master_demonologist->effectN( 1 ).percent();
      if ( active_demon == DEMON_SUCCUBUS && school == SCHOOL_SHADOW )
        m *= 1.0 + talent.master_demonologist->effectN( 3 ).percent();
    }
  }
  return m;
}

// Demonic Knowledge (412732): + rankens andel (33/67/100 %) af warlockens level i spell damage, mens en daemon er ude.
double warlock_t::composite_spell_power( school_e school ) const
{
  double sp = player_t::composite_spell_power( school );
  if ( active_demon != DEMON_NONE && talent.demonic_knowledge.ok() )
    sp += level() * talent.demonic_knowledge->effectN( 1 ).percent();
  return sp;
}

action_t* warlock_t::create_action( util::string_view name, util::string_view options_str )
{
  if ( name == "shadow_bolt" )
    return new shadow_bolt_t( this, options_str );
  if ( name == "immolate" )
    return new immolate_t( this, options_str );
  if ( name == "corruption" )
    return new corruption_t( this, options_str );

  if ( name == "summon_imp" )
    return new summon_demon_t( name, this, find_spell( 688 ), DEMON_IMP, options_str );
  if ( name == "summon_voidwalker" )
    return new summon_demon_t( name, this, find_spell( 697 ), DEMON_VOIDWALKER, options_str );
  if ( name == "summon_succubus" )
    return new summon_demon_t( name, this, find_spell( 712 ), DEMON_SUCCUBUS, options_str );
  if ( name == "summon_felhunter" )
    return new summon_demon_t( name, this, find_spell( 691 ), DEMON_FELHUNTER, options_str );
  if ( name == "summon_demon" )
    return new summon_demon_t( name, this, find_spell( 688 ), DEMON_NONE, options_str );
  if ( name == "demonic_sacrifice" )
    return new demonic_sacrifice_t( this, options_str );

  if ( name == "shoot" )
    return new shoot_t( this, options_str );
  if ( name == "life_tap" )
    return new life_tap_t( this, options_str );
  if ( name == "bane_of_agony" )
    return new bane_of_agony_t( this, options_str );
  if ( name == "siphon_life" )
    return new siphon_life_t( this, options_str );
  if ( name == "shadowburn" )
    return new shadowburn_t( this, options_str );
  if ( name == "conflagrate" )
    return new conflagrate_t( this, options_str );
  if ( name == "incinerate" )
    return new incinerate_t( this, options_str );
  if ( name == "wrack" )
    return new wrack_t( this, options_str );

  return player_t::create_action( name, options_str );
}

void warlock_t::create_pets()
{
  player_t::create_pets();

  pets.imp = new pets::imp::imp_pet_t( sim, this );
  // AULA-RETTELSE: andele af creature-dps og spredning fra wowsims/forever pets.go. Voidwalkerens naerkamp er
  // halveret (damage_mod 0,5). Felhunterens naerkamp er hos wowsims en level 60-konstant (70-97, x 0,8); her
  // SKOENNET som samme andel af creature-dps (83,5 / 2 s / 60,45 = 0,691, +/- 16,2 %) paa alle level.
  auto vw = new pets::melee_demon_t( sim, this, "voidwalker", PET_VOIDWALKER, DEMON_VOIDWALKER, 0.494, 0.264 );
  vw->damage_mod = 0.5;
  pets.voidwalker = vw;
  pets.succubus = new pets::melee_demon_t( sim, this, "succubus", PET_SAYAAD, DEMON_SUCCUBUS, 0.698, 0.215 );
  auto fh = new pets::melee_demon_t( sim, this, "felhunter", PET_FELHUNTER, DEMON_FELHUNTER, 0.691, 0.162 );
  fh->damage_mod = 0.8;
  pets.felhunter = fh;
}

void warlock_t::init_base_stats()
{
  player_t::init_base_stats();

  base.attack_crit_chance = get_passive_player_value(0.02, "all_crit");
  base.spell_crit_chance = get_passive_player_value(0.01701, "all_crit");
}

void warlock_t::init_spells()
{
  player_t::init_spells();

  rank.corruption = { this,
    spell_container_t().set_spell(   172 ),
    spell_container_t().set_spell(  6222 ),
    spell_container_t().set_spell(  6223 ),
    spell_container_t().set_spell(  7648 ),
    spell_container_t().set_spell( 11671 ),
    spell_container_t().set_spell( 11672 ),
    spell_container_t().set_spell( 25311 ),
  };

  rank.firebolt = { this,
    spell_container_t().set_spell(  3110 ),  // 1
    spell_container_t().set_spell(  7799 ),  // 8
    spell_container_t().set_spell(  7800 ),  // 18
    spell_container_t().set_spell(  7801 ),  // 28
    spell_container_t().set_spell(  7802 ),  // 38
    spell_container_t().set_spell( 11762 ),  // 48
    spell_container_t().set_spell( 11763 ),  // 58
  };

  rank.immolate = { this,
    spell_container_t().set_spell(   348 ),
    spell_container_t().set_spell(   707 ),
    spell_container_t().set_spell(  1094 ),
    spell_container_t().set_spell(  2941 ),
    spell_container_t().set_spell( 11665 ),
    spell_container_t().set_spell( 11667 ),
    spell_container_t().set_spell( 11668 ),
    spell_container_t().set_spell( 25309 ),
  };

  rank.shadow_bolt = { this,
    spell_container_t().set_spell(   686 ),
    spell_container_t().set_spell(   695 ),
    spell_container_t().set_spell(   705 ),
    spell_container_t().set_spell(  1088 ),
    spell_container_t().set_spell(  1106 ),
    spell_container_t().set_spell(  7641 ),
    spell_container_t().set_spell( 11659 ),
    spell_container_t().set_spell( 11660 ),
    spell_container_t().set_spell( 11661 ),
    spell_container_t().set_spell( 25307 ),
  };

  talent.improved_life_tap    = find_talent_spell( talent_tree::CLASS, "Improved Life Tap" );
  talent.siphon_life          = find_talent_spell( talent_tree::CLASS, "Siphon Life" );
  talent.shadowburn           = find_talent_spell( talent_tree::CLASS, "Shadowburn" );
  talent.conflagrate          = find_talent_spell( talent_tree::CLASS, "Conflagrate" );
  talent.incinerate           = find_talent_spell( talent_tree::CLASS, "Incinerate" );
  talent.wrack                = find_talent_spell( talent_tree::CLASS, "Wrack" );
  talent.improved_shadow_bolt = find_talent_spell( talent_tree::CLASS, "Improved Shadow Bolt" );
  talent.demonic_sacrifice    = find_talent_spell( talent_tree::CLASS, "Demonic Sacrifice" );
  talent.demonic_pact         = find_talent_spell( talent_tree::CLASS, "Demonic Pact" );
  talent.soul_link            = find_talent_spell( talent_tree::CLASS, "Soul Link" );
  talent.master_demonologist  = find_talent_spell( talent_tree::CLASS, "Master Demonologist" );
  talent.demonic_knowledge    = find_talent_spell( talent_tree::CLASS, "Demonic Knowledge" );
  talent.unholy_power         = find_talent_spell( talent_tree::CLASS, "Unholy Power" );
  talent.improved_imp         = find_talent_spell( talent_tree::CLASS, "Improved Imp" );
  talent.improved_sayaad      = find_talent_spell( talent_tree::CLASS, "Improved Sayaad" );
  talent.demonic_energies     = find_talent_spell( talent_tree::CLASS, "Demonic Energies" );
  soul_link_effect            = find_spell( 25228 );

  rank.life_tap = { this,
    spell_container_t().set_spell(  1454 ),  // 6
    spell_container_t().set_spell(  1455 ),  // 16
    spell_container_t().set_spell(  1456 ),  // 26
    spell_container_t().set_spell( 11687 ),  // 36
    spell_container_t().set_spell( 11688 ),  // 46
    spell_container_t().set_spell( 11689 ),  // 56
  };

  rank.bane_of_agony = { this,
    spell_container_t().set_spell(   980 ),  // 8
    spell_container_t().set_spell(  1014 ),  // 18
    spell_container_t().set_spell(  6217 ),  // 28
    spell_container_t().set_spell( 11711 ),  // 38
    spell_container_t().set_spell( 11712 ),  // 48
    spell_container_t().set_spell( 11713 ),  // 58
  };

  rank.siphon_life = { this, talent.siphon_life,
    spell_container_t().set_spell( 18265 ),  // 30
    spell_container_t().set_spell( 18879 ),  // 38
    spell_container_t().set_spell( 18880 ),  // 48
    spell_container_t().set_spell( 18881 ),  // 58
  };

  rank.shadowburn = { this, talent.shadowburn,
    spell_container_t().set_spell( 17877 ),  // 20
    spell_container_t().set_spell( 18867 ),  // 24
    spell_container_t().set_spell( 18868 ),  // 32
    spell_container_t().set_spell( 18869 ),  // 40
    spell_container_t().set_spell( 18870 ),  // 48
    spell_container_t().set_spell( 18871 ),  // 56
  };

  rank.conflagrate = { this, talent.conflagrate,
    spell_container_t().set_spell( 1293817 ),  // 25
    spell_container_t().set_spell( 1293818 ),  // 32
    spell_container_t().set_spell(   17962 ),  // 40
    spell_container_t().set_spell(   18930 ),  // 48
    spell_container_t().set_spell(   18931 ),  // 54
    spell_container_t().set_spell(   18932 ),  // 60
  };

  rank.incinerate = { this, talent.incinerate,
    spell_container_t().set_spell(  412758 ),  // 40
    spell_container_t().set_spell( 1293812 ),  // 50
    spell_container_t().set_spell( 1293813 ),  // 60
  };

  rank.wrack = { this, talent.wrack,
    spell_container_t().set_spell( 1316697 ),  // 40
  };

  rank.lash_of_pain = { this,
    spell_container_t().set_spell(  7814 ),  // 20
    spell_container_t().set_spell(  7815 ),  // 28
    spell_container_t().set_spell(  7816 ),  // 36
    spell_container_t().set_spell( 11778 ),  // 44
    spell_container_t().set_spell( 11779 ),  // 52
    spell_container_t().set_spell( 11780 ),  // 60
  };

  parse_all_class_passives();
  parse_all_passive_talents();

  choose_demons();
}

void warlock_t::create_buffs()
{
  player_t::create_buffs();

  buff.shadow_vulnerability = make_buff_fallback( talent.improved_shadow_bolt.ok(), this, "shadow_vulnerability",
                                                  find_spell( 17794 ) );
  buff.burning_shadow = make_buff( this, "burning_shadow", find_spell( 18789 ) )
                          ->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER );
  buff.touch_of_fire  = make_buff( this, "touch_of_fire", find_spell( 18791 ) )
                          ->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER );
}

void warlock_t::init_gains()
{
  player_t::init_gains();

  gains.life_tap = get_gain( "life_tap" );
  gains.life_tap_demon = get_gain( "life_tap_demonic_energies" );
}

void warlock_t::init_action_list()
{
  if ( !action_list_str.empty() )
  {
    player_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  warlock_apl::warlock( this );
  use_default_action_list = true;
  player_t::init_action_list();
}

}  // namespace warlock

const module_t* module_t::warlock()
{
  static warlock::warlock_module_t m;
  return &m;
}
