// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
//
// AULA-RETTELSE (simc-worker/patches/rogue.patch i AULA-Guild):
// Grov basismodel for Forever-rogue (Assassination-rotation), bygget efter
// warlock-/warrior-/mage-modulernes skabelon. Energi (regen fra power_type_data)
// og combo points; builders (Sinister Strike, Mutilate, Shank), finishers
// (Eviscerate, Slice and Dice, Rupture), aabning fra Stealth (Ambush/Garrote),
// Instant/Deadly Poison som vaabenprocs. Rank vaelges efter spillerens level.
// Talenternes passive effekter laeses fra spildata (parse_all_passive_talents);
// enkelte proc-/buff-talenter er haandskrevet (se PATCHES.md).
// Alle tal kommer fra spildata og vaabendata, undtagen de klassiske basis-AP-
// konstanter (som i hunter-/shaman-modulet) og varighederne pr. combo point,
// der staar i spillets beskrivelsestekst.
// Fjernes naar upstream selv implementerer klassen.

#include "simulationcraft.hpp"
#include "action/parse_effects.hpp"

#include "class_modules/apl/rogue.hpp"

namespace rogue
{
struct rogue_t final : public parse_player_effects_t
{
  rogue_t( sim_t* sim, std::string_view name, race_e r = RACE_NONE ) :
    parse_player_effects_t( sim, ROGUE, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_ENERGY; }

  std::string default_potion() const override { return rogue_apl::potion( this ); }
  std::string default_flask() const override  { return rogue_apl::flask( this ); }
  std::string default_food() const override   { return rogue_apl::food( this ); }

  action_t* create_action( std::string_view name, std::string_view options_str ) override;
  void init_action_list() override;
  void init_base_stats() override;
  void init_spells() override;
  void init_gains() override;
  void init_procs() override;
  void create_actions() override;
  void create_buffs() override;
  void init_finished() override;
  std::unique_ptr<expr_t> create_expression( std::string_view expression_str ) override;

  bool weapon_is( slot_e slot, weapon_e type ) const
  { return equipped_weapons.at( slot ).type == type; }
  bool has_off_hand() const
  { return equipped_weapons.at( SLOT_OFF_HAND ).type != WEAPON_NONE && equipped_weapons.at( SLOT_OFF_HAND ).max_dmg > 0; }

  void trigger_poisons( const action_state_t* s, const weapon_t* w );
  void trigger_seal_fate( const action_state_t* s );
  void trigger_relentless_strikes( int cp, action_t* a );

  struct
  {
    buff_t* adrenaline_rush = nullptr;
    buff_t* blade_flurry    = nullptr;
    buff_t* cold_blood      = nullptr;
    buff_t* slice_and_dice  = nullptr;
    buff_t* venom           = nullptr;
  } buff;

  struct
  {
    player_talent_t adrenaline_rush;
    player_talent_t blade_flurry;
    player_talent_t cold_blood;
    player_talent_t dual_wield_specialization;
    player_talent_t hack_and_slash;
    player_talent_t improved_slice_and_dice;
    player_talent_t mutilate;
    player_talent_t relentless_strikes;
    player_talent_t seal_fate;
    player_talent_t venom;
  } talent;

  struct
  {
    gain_t* relentless_strikes = nullptr;
    gain_t* seal_fate          = nullptr;
  } gain;

  struct
  {
    proc_t* seal_fate = nullptr;
    proc_t* instant_poison = nullptr;
    proc_t* deadly_poison  = nullptr;
  } proc;

  cooldown_t* seal_fate_icd = nullptr;

  // Gifte: spell = vaabenbelaegningen (proc-chance), attack = skaden (Instant) / DoT'en (Deadly)
  struct
  {
    spell_ranks_t ambush;
    spell_ranks_t deadly_poison;
    spell_ranks_t eviscerate;
    spell_ranks_t garrote;
    spell_ranks_t instant_poison;
    spell_ranks_t mutilate;
    spell_ranks_t rupture;
    spell_ranks_t shank;
    spell_ranks_t sinister_strike;
    spell_ranks_t slice_and_dice;
  } rank;

  action_t* instant_poison = nullptr;
  action_t* deadly_poison  = nullptr;
  const spell_data_t* instant_poison_enchant = spell_data_t::nil();
  const spell_data_t* deadly_poison_enchant  = spell_data_t::nil();
};

// ROGUE MODULE INTERFACE ===================================================

struct rogue_module_t final : public module_t
{
public:
  rogue_module_t() : module_t( ROGUE ) {}

  player_t* create_player( sim_t* sim, std::string_view name, race_e r = RACE_NONE ) const override
  {
    return new rogue_t( sim, name, r );
  }

  bool valid() const override { return true; }

  void register_hotfixes() const override {}

  void register_actor_initializers( sim_t* ) const override {}
};

template <class Base>
struct rogue_action_t : public parse_action_effects_t<Base>
{
private:
  using ab = parse_action_effects_t<Base>;

public:
  using base_t = rogue_action_t<Base>;

  bool finisher         = false;  // bruger alle combo points (mindst 1)
  bool requires_stealth = false;  // aabning fra Stealth: kun foer kampen
  bool cold_blood       = false;  // Cold Blood (+100 % crit) virker paa handlingen
  bool weapon_strike    = false;  // vaabenslag: kan udloese gift og Hack and Slash
  int cp                = 0;      // combo points brugt ved seneste finisher

  template <typename... Args>
  rogue_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {}

  rogue_t* p()
  { return debug_cast<rogue_t*>( this->player ); }

  const rogue_t* p() const
  { return debug_cast<const rogue_t*>( this->player ); }

  bool ready() override
  {
    if ( finisher && p()->resources.current[ RESOURCE_COMBO_POINT ] < 1 )
      return false;
    if ( requires_stealth && this->player->in_combat )
      return false;
    return ab::ready();
  }

  // "Discount Power On Miss": 80 % af energien refunderes ved miss/dodge/parry (som warriorens rage)
  void consume_resource() override
  {
    resource_e cr = this->current_resource();

    if ( cr == RESOURCE_ENERGY && this->cost() > 0 && this->data().flags( spell_attribute::SX_DISCOUNT_ON_MISS ) &&
         !this->proc && this->execute_state && !this->result_is_hit( this->execute_state->result ) )
    {
      this->last_resource_cost = 0.2 * this->cost();
      this->player->resource_loss( cr, this->last_resource_cost, nullptr, this );
      this->stats->consume_resource( cr, this->last_resource_cost );
      this->sim->print_log( "{} consumes {} {} for {} ({})", *this->player, this->last_resource_cost, cr, *this,
                            this->player->resources.current[ cr ] );
    }
    else
    {
      ab::consume_resource();
    }
  }

  void execute() override
  {
    if ( finisher )
      cp = as<int>( p()->resources.current[ RESOURCE_COMBO_POINT ] );

    ab::execute();

    if ( finisher && this->result_is_hit( this->execute_state->result ) )
    {
      p()->resource_loss( RESOURCE_COMBO_POINT, cp, nullptr, this );
      p()->trigger_relentless_strikes( cp, this );
    }

    // Cold Blood bruges af naeste beroerte handling (Mutilates to slag deler den; Mutilate selv fjerner den)
    if ( cold_blood && !this->background && p()->buff.cold_blood->check() )
      p()->buff.cold_blood->expire();
  }

  void impact( action_state_t* s ) override
  {
    ab::impact( s );

    if ( weapon_strike && this->result_is_hit( s->result ) )
      p()->trigger_poisons( s, this->weapon );
  }

  double composite_crit_chance() const override
  {
    double c = ab::composite_crit_chance();

    if ( cold_blood && p()->buff.cold_blood->check() )
      c += p()->buff.cold_blood->data().effectN( 1 ).percent();

    // Hack and Slash, dolk/naeve: +crit med vaabnet
    if ( this->weapon && p()->talent.hack_and_slash.ok() &&
         ( this->weapon->type == WEAPON_DAGGER || this->weapon->type == WEAPON_FIST ) )
      c += p()->talent.hack_and_slash->effectN( 1 ).percent();

    return c;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = ab::composite_da_multiplier( s );

    // Dual Wield Specialization (aura 122, uden for simcs passivliste): +skade med off-hand
    if ( this->weapon && this->weapon->slot == SLOT_OFF_HAND && p()->talent.dual_wield_specialization.ok() )
      m *= 1.0 + p()->talent.dual_wield_specialization->effectN( 1 ).percent();

    return m;
  }
};

using rogue_attack_t = rogue_action_t<melee_attack_t>;
using rogue_spell_t  = rogue_action_t<spell_t>;

// Kraever en dolk i hovedhaanden (Ambush, Mutilate): ellers "ikke laert" (stille baggrundshandling,
// i stedet for simcs fejlmeddelelse om forkert vaabentype).
static const spell_data_t* if_dagger( rogue_t* p, const spell_data_t* s )
{
  return p->weapon_is( SLOT_MAIN_HAND, WEAPON_DAGGER ) ? s : spell_data_t::not_found();
}

/////////////////////////////////////////////////////////////////////////////////////
//
// Gifte (Instant Poison / Deadly Poison som vaabenprocs)
//
/////////////////////////////////////////////////////////////////////////////////////

struct instant_poison_t : public rogue_spell_t
{
  instant_poison_t( rogue_t* p ) :
    rogue_spell_t( "instant_poison", p, p->rank.instant_poison.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->attack() )
  {
    background = proc = true;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = rogue_spell_t::composite_da_multiplier( s );
    if ( p()->buff.venom->check() )
      m *= 1.0 + p()->buff.venom->data().effectN( 1 ).percent();
    return m;
  }
};

struct deadly_poison_t : public rogue_spell_t
{
  deadly_poison_t( rogue_t* p ) :
    rogue_spell_t( "deadly_poison", p, p->rank.deadly_poison.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->attack() )
  {
    background = proc = true;
    // 12 s, tick hvert 3. s, op til 5 stakke (spildata); ny paafoering forlaenger og stakker
    dot_behavior = DOT_REFRESH_DURATION;
  }

  double composite_ta_multiplier( const action_state_t* s ) const override
  {
    double m = rogue_spell_t::composite_ta_multiplier( s );
    if ( p()->buff.venom->check() )
      m *= 1.0 + p()->buff.venom->data().effectN( 2 ).percent();
    return m;
  }
};

void rogue_t::trigger_poisons( const action_state_t* s, const weapon_t* w )
{
  if ( !w || !s->target )
    return;

  // Hovedhaand: Instant Poison. Off-hand: Deadly Poison, naar den er laert, ellers Instant Poison.
  action_t* poison           = instant_poison;
  const spell_data_t* coat   = instant_poison_enchant;
  proc_t* pr                 = proc.instant_poison;
  if ( w->slot == SLOT_OFF_HAND && deadly_poison )
  {
    poison = deadly_poison;
    coat   = deadly_poison_enchant;
    pr     = proc.deadly_poison;
  }
  if ( !poison || !coat->ok() )
    return;

  // Belaegningens proc-chance fra spildata; Improved Poisons' "Spell Proc Chance"-modifikator er allerede lagt
  // paa af parse_all_passive_talents(). Venom (fra Shank) laegges til haandkodet, da den er en buff.
  double chance = coat->proc_chance();
  if ( buff.venom->check() )
    chance += buff.venom->data().effectN( 3 ).percent();

  sim->print_debug( "{} poison {} proc chance {} (coat={})", *this, poison->name(), chance, coat->proc_chance() );
  if ( rng().roll( chance ) )
  {
    pr->occur();
    poison->execute_on_target( s->target );
  }
}

void rogue_t::trigger_seal_fate( const action_state_t* s )
{
  if ( !talent.seal_fate.ok() || s->result != RESULT_CRIT || seal_fate_icd->down() )
    return;

  if ( rng().roll( talent.seal_fate->effectN( 1 ).percent() ) )
  {
    proc.seal_fate->occur();
    resource_gain( RESOURCE_COMBO_POINT, 1, gain.seal_fate );
    seal_fate_icd->start( talent.seal_fate->internal_cooldown() );
  }
}

void rogue_t::trigger_relentless_strikes( int cp, action_t* a )
{
  if ( !talent.relentless_strikes.ok() || cp <= 0 )
    return;

  // "$m1% chance per Combo Point to restore $14181s1 Energy"
  if ( rng().roll( talent.relentless_strikes->effectN( 1 ).percent() * cp ) )
    resource_gain( RESOURCE_ENERGY, find_spell( 14181 )->effectN( 1 ).base_value(), gain.relentless_strikes, a );
}

/////////////////////////////////////////////////////////////////////////////////////
//
// Auto-attack (hovedhaand + off-hand)
//
/////////////////////////////////////////////////////////////////////////////////////

struct rogue_melee_t : public rogue_attack_t
{
  rogue_melee_t( std::string_view name, rogue_t* p, weapon_t* w ) :
    rogue_attack_t( name, p, spell_data_t::nil() )
  {
    weapon            = w;
    weapon_multiplier = 1.0;
    base_execute_time = w->swing_time;
    school            = SCHOOL_PHYSICAL;
    trigger_gcd       = 0_ms;

    background = repeating = true;
    may_glance = may_crit = may_dodge = may_parry = true;
    special       = false;
    weapon_strike = true;
  }
};

struct auto_attack_t : public rogue_attack_t
{
  auto_attack_t( rogue_t* p, std::string_view options_str ) :
    rogue_attack_t( "auto_attack", p, spell_data_t::nil() )
  {
    parse_options( options_str );
    trigger_gcd           = 0_ms;
    ignore_false_positive = true;

    assert( p->equipped_weapons[ SLOT_MAIN_HAND ].type != WEAPON_NONE );

    p->main_hand_attack = new rogue_melee_t( "melee_main_hand", p, &p->equipped_weapons[ SLOT_MAIN_HAND ] );
    if ( p->has_off_hand() )
      p->off_hand_attack = new rogue_melee_t( "melee_off_hand", p, &p->equipped_weapons[ SLOT_OFF_HAND ] );
  }

  void execute() override
  {
    player->main_hand_attack->schedule_execute();
    if ( player->off_hand_attack )
      player->off_hand_attack->schedule_execute();
  }

  bool ready() override
  {
    if ( player->is_moving() )
      return false;
    return player->main_hand_attack->execute_event == nullptr;
  }
};

/////////////////////////////////////////////////////////////////////////////////////
//
// Builders
//
/////////////////////////////////////////////////////////////////////////////////////

// Sinister Strike: vaabenskade + fast bonus (Normalized Weapon Damage), 1 combo point (Energize fra spildata)
struct sinister_strike_t : public rogue_attack_t
{
  sinister_strike_t( rogue_t* p, std::string_view options_str ) :
    rogue_attack_t( "sinister_strike", p, p->rank.sinister_strike.find_rank()->spell() )
  {
    parse_options( options_str );
    cold_blood = weapon_strike = true;
  }

  void impact( action_state_t* s ) override
  {
    rogue_attack_t::impact( s );
    p()->trigger_seal_fate( s );
  }
};

// Mutilate (talent, level 40): slag med begge vaaben (to udloeste spells), 2 combo points.
// Et hit-slag for hele angrebet; hvert vaaben ruller selv crit. +20 % mod forgiftet maal (effekt 4).
struct mutilate_strike_t : public rogue_attack_t
{
  double poisoned_bonus;

  mutilate_strike_t( std::string_view name, rogue_t* p, const spell_data_t* s, weapon_t* w, double bonus ) :
    rogue_attack_t( name, p, s ), poisoned_bonus( bonus )
  {
    background = dual = true;
    may_miss = may_dodge = may_parry = false;
    weapon = w;
    cold_blood = weapon_strike = true;
  }

  double composite_target_multiplier( player_t* t ) const override
  {
    double m = rogue_attack_t::composite_target_multiplier( t );
    if ( p()->deadly_poison && p()->deadly_poison->get_dot( t )->is_ticking() )
      m *= 1.0 + poisoned_bonus;
    return m;
  }

  void impact( action_state_t* s ) override
  {
    rogue_attack_t::impact( s );
    p()->trigger_seal_fate( s );
  }
};

struct mutilate_t : public rogue_attack_t
{
  action_t* mh = nullptr;
  action_t* oh = nullptr;

  mutilate_t( rogue_t* p, std::string_view options_str ) :
    rogue_attack_t( "mutilate", p,
                    p->talent.mutilate.ok() ? if_dagger( p, p->rank.mutilate.find_rank()->spell() )
                                            : spell_data_t::not_found() )
  {
    parse_options( options_str );
    may_crit = false;
    weapon_multiplier = 0;
    base_dd_min = base_dd_max = 0;

    if ( data().ok() )
    {
      mh = new mutilate_strike_t( "mutilate_mh", p, data().effectN( 2 ).trigger(), &p->equipped_weapons[ SLOT_MAIN_HAND ],
                                 data().effectN( 4 ).percent() );
      add_child( mh );
      if ( p->has_off_hand() )
      {
        oh = new mutilate_strike_t( "mutilate_oh", p, data().effectN( 3 ).trigger(), &p->equipped_weapons[ SLOT_OFF_HAND ],
                                   data().effectN( 4 ).percent() );
        add_child( oh );
      }
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();

    if ( result_is_hit( execute_state->result ) )
    {
      mh->execute_on_target( execute_state->target );
      if ( oh )
        oh->execute_on_target( execute_state->target );
    }

    if ( p()->buff.cold_blood->check() )
      p()->buff.cold_blood->expire();
  }
};

// Shank (level 24): off-hand-slag, 50 % vaabenskade, +15 energi (Flash of Steel via spildata), kan ikke misse.
struct shank_t : public rogue_attack_t
{
  shank_t( rogue_t* p, std::string_view options_str ) :
    rogue_attack_t( "shank", p, p->has_off_hand() ? p->rank.shank.find_rank()->spell() : spell_data_t::not_found() )
  {
    parse_options( options_str );
    weapon   = &p->equipped_weapons[ SLOT_OFF_HAND ];
    may_miss = false;
    weapon_strike = true;
  }

  void execute() override
  {
    rogue_attack_t::execute();

    if ( p()->talent.venom.ok() )
      p()->buff.venom->trigger();
  }
};

/////////////////////////////////////////////////////////////////////////////////////
//
// Aabning fra Stealth (kun foer kampen)
//
/////////////////////////////////////////////////////////////////////////////////////

struct ambush_t : public rogue_attack_t
{
  ambush_t( rogue_t* p, std::string_view options_str ) :
    rogue_attack_t( "ambush", p, if_dagger( p, p->rank.ambush.find_rank()->spell() ) )
  {
    parse_options( options_str );
    requires_stealth = cold_blood = weapon_strike = true;
  }

  void impact( action_state_t* s ) override
  {
    rogue_attack_t::impact( s );
    p()->trigger_seal_fate( s );
  }
};

struct garrote_t : public rogue_attack_t
{
  garrote_t( rogue_t* p, std::string_view options_str ) :
    rogue_attack_t( "garrote", p, p->rank.garrote.find_rank()->spell() )
  {
    parse_options( options_str );
    requires_stealth = true;
  }

  // Bleed: rustning gaelder ikke
  double composite_target_armor( const action_state_t* ) const override
  { return 0.0; }
};

/////////////////////////////////////////////////////////////////////////////////////
//
// Finishers
//
/////////////////////////////////////////////////////////////////////////////////////

// Eviscerate: effekt 1 = grundskade + "Points Per Combo Point" x combo points
struct eviscerate_t : public rogue_attack_t
{
  eviscerate_t( rogue_t* p, std::string_view options_str ) :
    rogue_attack_t( "eviscerate", p, p->rank.eviscerate.find_rank()->spell() )
  {
    parse_options( options_str );
    finisher = cold_blood = true;
  }

  double base_da_min( const action_state_t* s ) const override
  { return rogue_attack_t::base_da_min( s ) + cp * data().effectN( 1 ).pp_combo_points(); }

  double base_da_max( const action_state_t* s ) const override
  { return rogue_attack_t::base_da_max( s ) + cp * data().effectN( 1 ).pp_combo_points(); }
};

// Slice and Dice: +20 % (rank 2: +30 %) auto-attack-hastighed. Varighed efter beskrivelsen:
// 9/12/15/18/21 s for 1-5 combo points = spildataens 6 s + 3 s pr. combo point, alt ganget med Improved Slice and Dice.
struct slice_and_dice_t : public rogue_spell_t
{
  slice_and_dice_t( rogue_t* p, std::string_view options_str ) :
    rogue_spell_t( "slice_and_dice", p, p->rank.slice_and_dice.find_rank()->spell() )
  {
    parse_options( options_str );
    finisher = true;
    harmful = may_miss = may_crit = false;
    target = p;
  }

  void execute() override
  {
    rogue_spell_t::execute();

    // data().duration() har allerede Improved Slice and Dice (Spell Duration-modifikator via
    // parse_all_passive_talents); de 3 s pr. combo point ganges med samme faktor: (6 + 3 x cp) x (1 + talent).
    timespan_t per_cp = timespan_t::from_seconds( 3 );
    if ( p()->talent.improved_slice_and_dice.ok() )
      per_cp *= 1.0 + p()->talent.improved_slice_and_dice->effectN( 1 ).percent();
    p()->buff.slice_and_dice->trigger( data().duration() + per_cp * cp );
  }
};

// Rupture: tick hvert 2. s = grundvaerdi + "Points Per Combo Point" x combo points.
// Varighed efter beskrivelsen: 8/10/12/14/16 s = spildataens 6 s + 2 s pr. combo point.
struct rupture_t : public rogue_attack_t
{
  rupture_t( rogue_t* p, std::string_view options_str ) :
    rogue_attack_t( "rupture", p, p->rank.rupture.find_rank()->spell() )
  {
    parse_options( options_str );
    finisher = true;
    dot_behavior = DOT_CLIP;
  }

  timespan_t composite_dot_duration( const action_state_t* s ) const override
  { return rogue_attack_t::composite_dot_duration( s ) + timespan_t::from_seconds( 2 ) * cp; }

  double base_ta( const action_state_t* s ) const override
  { return rogue_attack_t::base_ta( s ) + cp * data().effectN( 1 ).pp_combo_points(); }

  // Bleed: rustning gaelder ikke
  double composite_target_armor( const action_state_t* ) const override
  { return 0.0; }
};

/////////////////////////////////////////////////////////////////////////////////////
//
// Talent-cooldowns
//
/////////////////////////////////////////////////////////////////////////////////////

struct buff_spell_t : public rogue_spell_t
{
  buff_t* buff;

  buff_spell_t( std::string_view n, rogue_t* p, const player_talent_t& t, buff_t* b, std::string_view options_str ) :
    rogue_spell_t( n, p, t.ok() ? t.spell() : spell_data_t::not_found() ), buff( b )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = false;
    target = p;
  }

  void execute() override
  {
    rogue_spell_t::execute();
    buff->trigger();
  }
};

/////////////////////////////////////////////////////////////////////////////////////
//
// Spiller
//
/////////////////////////////////////////////////////////////////////////////////////

action_t* rogue_t::create_action( std::string_view name, std::string_view options_str )
{
  if ( name == "auto_attack" )     return new auto_attack_t( this, options_str );
  if ( name == "sinister_strike" ) return new sinister_strike_t( this, options_str );
  if ( name == "mutilate" )        return new mutilate_t( this, options_str );
  if ( name == "shank" )           return new shank_t( this, options_str );
  if ( name == "ambush" )          return new ambush_t( this, options_str );
  if ( name == "garrote" )         return new garrote_t( this, options_str );
  if ( name == "eviscerate" )      return new eviscerate_t( this, options_str );
  if ( name == "slice_and_dice" )  return new slice_and_dice_t( this, options_str );
  if ( name == "rupture" )         return new rupture_t( this, options_str );
  if ( name == "cold_blood" )
    return new buff_spell_t( name, this, talent.cold_blood, buff.cold_blood, options_str );
  if ( name == "adrenaline_rush" )
    return new buff_spell_t( name, this, talent.adrenaline_rush, buff.adrenaline_rush, options_str );
  if ( name == "blade_flurry" )
    return new buff_spell_t( name, this, talent.blade_flurry, buff.blade_flurry, options_str );

  return parse_player_effects_t::create_action( name, options_str );
}

// learned.<evne>: er evnen laert paa spillerens level (bruges i rotationen)
std::unique_ptr<expr_t> rogue_t::create_expression( std::string_view expression_str )
{
  auto splits = util::string_split<std::string_view>( expression_str, "." );
  if ( splits.size() == 2 && splits[ 0 ] == "learned" )
  {
    spell_ranks_t* r = nullptr;
    if ( splits[ 1 ] == "slice_and_dice" ) r = &rank.slice_and_dice;
    if ( splits[ 1 ] == "rupture" )        r = &rank.rupture;
    if ( r )
      return expr_t::create_constant( expression_str, r->find_rank()->spell()->ok() );
  }

  return parse_player_effects_t::create_expression( expression_str );
}

void rogue_t::init_base_stats()
{
  // Klassiske konstanter som i hunter-/shaman-modulet (ikke i spildata): AP = 2 x level - 20 + Str + Agi
  base.attack_power_per_strength = 1.0;
  base.attack_power_per_agility  = 1.0;

  parse_player_effects_t::init_base_stats();

  base.stats.attack_power = 2.0 * level() - 20.0;
}

void rogue_t::init_gains()
{
  parse_player_effects_t::init_gains();

  gain.relentless_strikes = get_gain( "relentless_strikes" );
  gain.seal_fate          = get_gain( "seal_fate" );
}

void rogue_t::init_procs()
{
  parse_player_effects_t::init_procs();

  proc.seal_fate      = get_proc( "seal_fate" );
  proc.instant_poison = get_proc( "instant_poison" );
  proc.deadly_poison  = get_proc( "deadly_poison" );
}

void rogue_t::create_actions()
{
  // Giftene er laert efter klassequesten (level 20, Deadly Poison 30); rank efter level
  auto ip = rank.instant_poison.find_rank();
  if ( ip->spell()->ok() )
  {
    instant_poison_enchant = ip->spell();
    instant_poison         = new instant_poison_t( this );
  }
  auto dp = rank.deadly_poison.find_rank();
  if ( dp->spell()->ok() )
  {
    deadly_poison_enchant = dp->spell();
    deadly_poison         = new deadly_poison_t( this );
  }

  parse_player_effects_t::create_actions();
}

void rogue_t::create_buffs()
{
  parse_player_effects_t::create_buffs();

  auto const snd = rank.slice_and_dice.find_rank()->spell();
  buff.slice_and_dice = make_buff_fallback( snd->ok(), this, "slice_and_dice", snd )
                          ->set_cooldown( 0_ms );

  buff.cold_blood = make_buff_fallback( talent.cold_blood.ok(), this, "cold_blood", talent.cold_blood )
                      ->set_cooldown( 0_ms );

  buff.adrenaline_rush = make_buff_fallback( talent.adrenaline_rush.ok(), this, "adrenaline_rush", talent.adrenaline_rush )
                           ->set_cooldown( 0_ms )
                           ->set_affects_regen( true );

  buff.blade_flurry = make_buff_fallback( talent.blade_flurry.ok(), this, "blade_flurry", talent.blade_flurry )
                        ->set_cooldown( 0_ms );

  buff.venom = make_buff_fallback( talent.venom.ok(), this, "venom", find_spell( 1310703 ) );

  seal_fate_icd = get_cooldown( "seal_fate_icd" );
}

void rogue_t::init_finished()
{
  // Slice and Dice / Blade Flurry (aura 319 auto-attack-hastighed) og Adrenaline Rush (aura 110 energi-regen)
  parse_effects( buff.slice_and_dice );
  parse_effects( buff.blade_flurry );
  parse_effects( buff.adrenaline_rush );

  parse_player_effects_t::init_finished();
}

void rogue_t::init_spells()
{
  parse_player_effects_t::init_spells();

  // kommentar = spell level
  rank.sinister_strike = { this,
    spell_container_t().set_spell(  1752 ),  // 1
    spell_container_t().set_spell(  1757 ),  // 6
    spell_container_t().set_spell(  1758 ),  // 14
    spell_container_t().set_spell(  1759 ),  // 22
    spell_container_t().set_spell(  1760 ),  // 30
    spell_container_t().set_spell(  8621 ),  // 38
    spell_container_t().set_spell( 11293 ),  // 46
    spell_container_t().set_spell( 11294 ),  // 54
  };

  rank.eviscerate = { this,
    spell_container_t().set_spell(  2098 ),  // 1
    spell_container_t().set_spell(  6760 ),  // 8
    spell_container_t().set_spell(  6761 ),  // 16
    spell_container_t().set_spell(  6762 ),  // 24
    spell_container_t().set_spell(  8623 ),  // 32
    spell_container_t().set_spell(  8624 ),  // 40
    spell_container_t().set_spell( 11299 ),  // 48
    spell_container_t().set_spell( 11300 ),  // 56
    spell_container_t().set_spell( 31016 ),  // 60
  };

  rank.slice_and_dice = { this,
    spell_container_t().set_spell(  5171 ),  // 10
    spell_container_t().set_spell(  6774 ),  // 42
  };

  rank.rupture = { this,
    spell_container_t().set_spell(  1943 ),  // 20
    spell_container_t().set_spell(  8639 ),  // 28
    spell_container_t().set_spell(  8640 ),  // 36
    spell_container_t().set_spell( 11273 ),  // 44
    spell_container_t().set_spell( 11274 ),  // 52
    spell_container_t().set_spell( 11275 ),  // 60
  };

  rank.garrote = { this,
    spell_container_t().set_spell(   703 ),  // 14
    spell_container_t().set_spell(  8631 ),  // 22
    spell_container_t().set_spell(  8632 ),  // 30
    spell_container_t().set_spell(  8633 ),  // 38
    spell_container_t().set_spell( 11289 ),  // 46
    spell_container_t().set_spell( 11290 ),  // 54
  };

  rank.ambush = { this,
    spell_container_t().set_spell(  8676 ),  // 18
    spell_container_t().set_spell(  8724 ),  // 26
    spell_container_t().set_spell(  8725 ),  // 34
    spell_container_t().set_spell( 11267 ),  // 42
    spell_container_t().set_spell( 11268 ),  // 50
    spell_container_t().set_spell( 11269 ),  // 58
  };

  rank.shank = { this,
    spell_container_t().set_spell( 1329283 ),  // 24
    spell_container_t().set_spell( 1329549 ),  // 44
  };

  // spell = driver (energi, combo points), hovedhaand-/off-hand-slag er dens Trigger Spell-effekter 2 og 3
  rank.mutilate = { this,
    spell_container_t().set_spell(  399956 ),  // 40
    spell_container_t().set_spell( 1241582 ),  // 50
    spell_container_t().set_spell( 1241584 ),  // 60
  };

  // spell = vaabenbelaegning (proc-chance), attack = skaden
  rank.instant_poison = { this,
    spell_container_t().set_spell(  8679 ).set_attack(  8680 ),  // 20
    spell_container_t().set_spell(  8686 ).set_attack(  8685 ),  // 28
    spell_container_t().set_spell(  8688 ).set_attack(  8689 ),  // 36
    spell_container_t().set_spell( 11338 ).set_attack( 11335 ),  // 44
    spell_container_t().set_spell( 11339 ).set_attack( 11336 ),  // 52
    spell_container_t().set_spell( 11340 ).set_attack( 11337 ),  // 60
  };

  rank.deadly_poison = { this,
    spell_container_t().set_spell(  2823 ).set_attack(  2818 ),  // 30
    spell_container_t().set_spell(  2824 ).set_attack(  2819 ),  // 38
    spell_container_t().set_spell( 11355 ).set_attack( 11353 ),  // 46
    spell_container_t().set_spell( 11356 ).set_attack( 11354 ),  // 54
    spell_container_t().set_spell( 25351 ).set_attack( 25349 ),  // 60
  };

  talent.adrenaline_rush           = find_talent_spell( talent_tree::CLASS, "Adrenaline Rush" );
  talent.blade_flurry              = find_talent_spell( talent_tree::CLASS, "Blade Flurry" );
  talent.cold_blood                = find_talent_spell( talent_tree::CLASS, "Cold Blood" );
  talent.dual_wield_specialization = find_talent_spell( talent_tree::CLASS, "Dual Wield Specialization" );
  talent.hack_and_slash            = find_talent_spell( talent_tree::CLASS, "Hack and Slash" );
  talent.improved_slice_and_dice   = find_talent_spell( talent_tree::CLASS, "Improved Slice and Dice" );
  talent.mutilate                  = find_talent_spell( talent_tree::CLASS, "Mutilate" );
  talent.relentless_strikes        = find_talent_spell( talent_tree::CLASS, "Relentless Strikes" );
  talent.seal_fate                 = find_talent_spell( talent_tree::CLASS, "Seal Fate" );
  talent.venom                     = find_talent_spell( talent_tree::CLASS, "Venom" );

  parse_all_class_passives();
  // Talenternes passive effekter direkte fra spildata (som warlock, warrior, mage m.fl.):
  // fx Malice (crit), Precision (hit), Lethality (crit-bonus), Aggression og Coup de Grace (skade),
  // Vile Poisons (giftskade), Flash of Steel (Shank), Blood Splatter, Vigor.
  parse_all_passive_talents();
}

void rogue_t::init_action_list()
{
  if ( !action_list_str.empty() )
  {
    parse_player_effects_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  rogue_apl::rogue( this );
  use_default_action_list = true;
  parse_player_effects_t::init_action_list();
}

}  // namespace rogue

const module_t* module_t::rogue()
{
  static rogue::rogue_module_t m;
  return &m;
}
