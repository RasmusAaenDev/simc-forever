// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
//
// AULA-RETTELSE (simc-worker/patches/paladin.patch i AULA-Guild):
// Grov basismodel for Forever-paladin (Retribution), bygget efter warrior- og
// mage-modulernes skabelon. Mana, auto-attack med vaabnet i main hand, Seal of
// Command / Seal of Righteousness (en seal ad gangen), Judgement (forbruger ikke
// seglet), Holy Strike, Hammer of Wrath (< 20 % liv) og Consecration.
// Talenternes passive effekter laeses fra spildata (parse_all_passive_talents);
// Vengeance, Sanctified Judgement og Two-Handed Weapon Specialization er
// haandskrevet. Unsealed Wrath, Seal of the Crusader og Exorcism er ikke med.
// Tal kommer fra spildata og vaabendata; undtagelser (Seal of Commands 7 PPM,
// formlen for AP og Seal of Righteousness pr. slag) er beskrevet i PATCHES.md.
// Fjernes naar upstream selv implementerer klassen.
/*
  TODO: reimplement Holy if anyone ever becomes interested in maintaining it
*/
#include "simulationcraft.hpp"
#include "action/dbc_proc_callback.hpp"
#include "action/parse_effects.hpp"
#include "item/special_effect.hpp"
#include <algorithm>

#include "class_modules/apl/paladin.hpp"

// ==========================================================================
// Paladin
// ==========================================================================
namespace paladin
{
// Seal of Command: spildataen har "Proc Chance 100 %" og 1 s ICD, men beskrivelsen siger "a chance",
// og spildataen eksporterer ikke Blizzards PPM-tabel. 7 procs pr. minut (efter vaabnets hastighed)
// er classic-vaerdien, som Icy Veins' Forever-guide ogsaa angiver. Antagelse, se PATCHES.md.
constexpr double SEAL_OF_COMMAND_PPM = 7.0;

struct paladin_t final : public parse_player_effects_t
{
  paladin_t( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) :
    parse_player_effects_t( sim, PALADIN, name, r ) {}

  resource_e primary_resource() const override { return RESOURCE_MANA; }

  std::string default_potion() const override { return paladin_apl::potion( this ); }
  std::string default_flask() const override  { return paladin_apl::flask( this ); }
  std::string default_food() const override   { return paladin_apl::food( this ); }

  action_t* create_action( util::string_view name, util::string_view options_str ) override;
  void create_actions() override;
  void init_action_list() override;
  void init_base_stats() override;
  void init_spells() override;
  void init_gains() override;
  void create_buffs() override;
  double composite_player_multiplier( school_e ) const override;

  struct
  {
    buff_t* seal_of_command       = nullptr;
    buff_t* seal_of_righteousness = nullptr;
    buff_t* vengeance             = nullptr;
  } buff;

  struct
  {
    player_talent_t seal_of_command;
    player_talent_t vengeance;
    player_talent_t sanctified_judgement;
    player_talent_t two_handed_weapon_specialization;
    player_talent_t improved_seals;
  } talent;

  struct
  {
    spell_ranks_t consecration;
    spell_ranks_t hammer_of_wrath;
    spell_ranks_t holy_strike;
    spell_ranks_t judgement;
    spell_ranks_t seal_of_command;
    spell_ranks_t seal_of_righteousness;
  } rank;

  struct
  {
    gain_t* sanctified_judgement = nullptr;
  } gain;

  // Baggrundshandlinger (procs og judgement-skade)
  struct
  {
    action_t* seal_of_command_proc        = nullptr;
    action_t* seal_of_righteousness_proc  = nullptr;
    action_t* judgement_of_command        = nullptr;
    action_t* judgement_of_righteousness  = nullptr;
    action_t* seal_of_command_cast        = nullptr;
    action_t* seal_of_righteousness_cast  = nullptr;
  } active;

  cooldown_t* seal_of_command_icd = nullptr;

  bool two_hander() const
  { return equipped_weapons.at( SLOT_MAIN_HAND ).group() == WEAPON_2H; }
};

// PALADIN MODULE INTERFACE =================================================

struct paladin_module_t : public module_t
{
  paladin_module_t() : module_t( PALADIN ) {}

  player_t* create_player( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) const override
  {
    auto p = new paladin_t( sim, name, r );
    return p;
  }

  bool valid() const override
  {
    return true;
  }

  void register_actor_initializers( sim_t* ) const override {}

  void register_hotfixes() const override {}
};

template <class Base>
struct paladin_action_t : public parse_action_effects_t<Base>
{
private:
  using ab = parse_action_effects_t<Base>;

public:
  using base_t = paladin_action_t<Base>;

  // Vengeance udloeses af ikke-periodiske kritiske slag (Consecrations ticks taeller ikke).
  bool triggers_vengeance = true;

  template <typename... Args>
  paladin_action_t( Args&&... args ) : ab( std::forward<Args>( args )... ) {}

  paladin_t* p()
  { return debug_cast<paladin_t*>( this->player ); }

  const paladin_t* p() const
  { return debug_cast<const paladin_t*>( this->player ); }

  void impact( action_state_t* s ) override
  {
    ab::impact( s );

    if ( triggers_vengeance && s->result == RESULT_CRIT && p()->talent.vengeance.ok() )
      p()->buff.vengeance->trigger();
  }
};

using paladin_attack_t = paladin_action_t<melee_attack_t>;
using paladin_spell_t  = paladin_action_t<spell_t>;

// ==========================================================================
// Seals
// ==========================================================================

// Seal of Command-proc (20424): Weapon Damage% 70 % (spildata) + 0,29 x spell power, Holy.
// Spell Type Melee: rulles paa angrebstabellen som et gult slag.
struct seal_of_command_proc_t : public paladin_attack_t
{
  seal_of_command_proc_t( paladin_t* player ) :
    paladin_attack_t( "seal_of_command_proc", player, player->find_spell( 20424 ) )
  {
    background = proc = true;
    trigger_gcd = 0_ms;
    // Weapon Damage%-effekten giver vaaben og multiplikator; SP-koefficienten laeses ikke af simc for vaabenskade.
    spell_power_mod.direct = data().effectN( 1 ).sp_coeff();
  }
};

// Seal of Righteousness pr. slag. Beskrivelsen (fx 20287): "${($s3/87+$<1h>)*$<mult>} to
// ${($s3/25+$<2h>)*$<mult>} Holy damage", $1h = 0,0765 x SPH, $2h = 0,12 x SPH, $mult = 1 + Improved Seals.
// Antagelse: nedre graense for enhaandsvaaben, oevre for tohaandsvaaben (ingen yderligere hastighedsafhaengighed).
struct seal_of_righteousness_proc_t : public paladin_spell_t
{
  seal_of_righteousness_proc_t( paladin_t* player ) :
    paladin_spell_t( "seal_of_righteousness_proc", player,
                     player->rank.seal_of_righteousness.find_rank()->spell() )
  {
    background = proc = true;
    may_miss = may_crit = false;  // ekstra skade paa et slag der allerede har ramt
    trigger_gcd = 0_ms;
    school = SCHOOL_HOLY;
    triggers_vengeance = false;

    const spell_data_t* seal = &data();
    double s3 = seal->ok() ? seal->effectN( 1 ).average( player, player->level() ) : 0.0;
    bool two_h = player->two_hander();
    base_dd_min = base_dd_max = s3 / ( two_h ? 25.0 : 87.0 );
    spell_power_mod.direct = two_h ? 0.12 : 0.0765;
    spell_power_mod.tick = 0;
    base_td = 0;
    dot_duration = 0_ms;
    base_tick_time = 0_ms;
  }

  double action_multiplier() const override
  {
    double m = paladin_spell_t::action_multiplier();
    // $mult i beskrivelsen; Improved Seals staar ikke paa sealens "Affecting Spells", saa den laegges paa her.
    if ( p()->talent.improved_seals.ok() )
      m *= 1.0 + p()->talent.improved_seals->effectN( 1 ).percent();
    return m;
  }
};

// Judgement of Command (20467 m.fl.): School Damage, "Always Hit". Seal of Commands beskrivelse:
// "$/2;20467s1 Holy damage, $20467s1 if the target is stunned or incapacitated" -> halv skade (antaget hele beloebet).
struct judgement_of_command_t : public paladin_attack_t
{
  judgement_of_command_t( paladin_t* player ) :
    paladin_attack_t( "judgement_of_command", player,
                      player->rank.seal_of_command.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->attack() )
  {
    background = true;
    trigger_gcd = 0_ms;
    may_miss = may_dodge = may_parry = may_block = may_glance = false;
    base_dd_multiplier *= 0.5;
  }
};

// Judgement of Righteousness (20187 m.fl.): School Damage s1 + 0,429 x SP (spildata).
struct judgement_of_righteousness_t : public paladin_attack_t
{
  judgement_of_righteousness_t( paladin_t* player ) :
    paladin_attack_t( "judgement_of_righteousness", player,
                      player->rank.seal_of_righteousness.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->attack() )
  {
    background = true;
    trigger_gcd = 0_ms;
    may_dodge = may_parry = may_block = may_glance = false;
  }
};

// Faelles for seals: en seal ad gangen (buff), 30 s, mana og GCD fra spildata.
struct seal_t : public paladin_spell_t
{
  bool command;

  seal_t( std::string_view name, paladin_t* player, const spell_data_t* s, bool is_command,
          std::string_view options_str ) :
    paladin_spell_t( name, player, s ), command( is_command )
  {
    parse_options( options_str );
    harmful = may_miss = may_crit = false;
    triggers_vengeance = false;
    target = player;
  }

  buff_t* seal_buff() const
  { return command ? p()->buff.seal_of_command : p()->buff.seal_of_righteousness; }

  void execute() override
  {
    paladin_spell_t::execute();

    for ( buff_t* b : { p()->buff.seal_of_command, p()->buff.seal_of_righteousness } )
      if ( b != seal_buff() )
        b->expire();
    seal_buff()->trigger();
  }
};

struct seal_of_command_t : public seal_t
{
  seal_of_command_t( paladin_t* player, std::string_view options_str ) :
    seal_t( "seal_of_command", player, player->rank.seal_of_command.find_rank()->spell(), true, options_str )
  {}
};

struct seal_of_righteousness_t : public seal_t
{
  seal_of_righteousness_t( paladin_t* player, std::string_view options_str ) :
    seal_t( "seal_of_righteousness", player, player->rank.seal_of_righteousness.find_rank()->spell(), false,
            options_str )
  {}
};

// ==========================================================================
// Melee
// ==========================================================================

struct paladin_melee_t : public paladin_attack_t
{
  paladin_melee_t( paladin_t* player, weapon_t* w ) :
    paladin_attack_t( "melee", player, spell_data_t::nil() )
  {
    weapon            = w;
    weapon_multiplier = 1.0;
    base_execute_time = w->swing_time;
    school            = SCHOOL_PHYSICAL;
    trigger_gcd       = 0_ms;

    background = repeating = true;
    may_glance = may_crit = may_dodge = may_parry = true;
    special    = false;
  }

  void impact( action_state_t* s ) override
  {
    paladin_attack_t::impact( s );

    if ( !result_is_hit( s->result ) )
      return;

    // Seals reagerer paa hvide slag ("Proc Flags: White Melee").
    if ( p()->buff.seal_of_righteousness->check() )
      p()->active.seal_of_righteousness_proc->execute_on_target( s->target );

    if ( p()->buff.seal_of_command->check() && p()->seal_of_command_icd->up() )
    {
      double chance = SEAL_OF_COMMAND_PPM * weapon->swing_time.total_seconds() / 60.0;
      if ( rng().roll( chance ) )
      {
        p()->active.seal_of_command_proc->execute_on_target( s->target );
        p()->seal_of_command_icd->start();
      }
    }
  }
};

struct auto_attack_t : public paladin_attack_t
{
  auto_attack_t( paladin_t* player, std::string_view options_str ) :
    paladin_attack_t( "auto_attack", player, spell_data_t::nil() )
  {
    parse_options( options_str );
    trigger_gcd           = 0_ms;
    ignore_false_positive = true;

    if ( player->equipped_weapons[ SLOT_MAIN_HAND ].type != WEAPON_NONE )
      player->main_hand_attack = new paladin_melee_t( player, &player->equipped_weapons[ SLOT_MAIN_HAND ] );
  }

  void execute() override
  {
    player->main_hand_attack->schedule_execute();
  }

  bool ready() override
  {
    if ( !player->main_hand_attack || player->is_moving() )
      return false;
    return player->main_hand_attack->execute_event == nullptr;
  }
};

// ==========================================================================
// Abilities
// ==========================================================================

// Judgement (20271): 6 % base mana, 10 s nedkoeling. Forbruger IKKE seglet ("Does not consume the Seal").
// Skaden er den aktive seals judgement-spell.
struct judgement_t : public paladin_spell_t
{
  judgement_t( paladin_t* player, std::string_view options_str ) :
    paladin_spell_t( "judgement", player, player->rank.judgement.find_rank()->spell() )
  {
    parse_options( options_str );
    may_miss = may_crit = false;
    triggers_vengeance = false;
  }

  action_t* judged() const
  {
    if ( p()->buff.seal_of_command->check() )
      return p()->active.judgement_of_command;
    if ( p()->buff.seal_of_righteousness->check() )
      return p()->active.judgement_of_righteousness;
    return nullptr;
  }

  bool ready() override
  {
    action_t* j = judged();
    if ( !j || !j->data().ok() )
      return false;
    return paladin_spell_t::ready();
  }

  void execute() override
  {
    action_t* j = judged();
    action_t* seal = p()->buff.seal_of_command->check() ? p()->active.seal_of_command_cast
                                                        : p()->active.seal_of_righteousness_cast;
    paladin_spell_t::execute();

    if ( j )
      j->execute_on_target( target );

    // Sanctified Judgement: $m1 % chance for at faa $m2 % af den doemte seals manapris igen.
    if ( p()->talent.sanctified_judgement.ok() && seal &&
         rng().roll( p()->talent.sanctified_judgement->effectN( 1 ).percent() ) )
    {
      p()->resource_gain( RESOURCE_MANA, p()->talent.sanctified_judgement->effectN( 2 ).percent() * seal->cost(),
                          p()->gain.sanctified_judgement, this );
    }
  }
};

// Holy Strike: Normalized Weapon Damage (flad vaerdi + normaliseret vaaben) x Weapon Damage% fra spildata,
// plus 0,429 x spell power (effekt 1's koefficient), Holy. 10 s nedkoeling.
struct holy_strike_t : public paladin_attack_t
{
  holy_strike_t( paladin_t* player, std::string_view options_str ) :
    paladin_attack_t( "holy_strike", player, player->rank.holy_strike.find_rank()->spell() )
  {
    parse_options( options_str );
    spell_power_mod.direct = data().effectN( 1 ).sp_coeff();
  }
};

// Hammer of Wrath: kun naar maalet har 20 % liv eller mindre. Skade, cast-tid og nedkoeling fra spildata.
struct hammer_of_wrath_t : public paladin_attack_t
{
  hammer_of_wrath_t( paladin_t* player, std::string_view options_str ) :
    paladin_attack_t( "hammer_of_wrath", player, player->rank.hammer_of_wrath.find_rank()->spell() )
  {
    parse_options( options_str );
    weapon = nullptr;
    weapon_multiplier = 0;
    may_dodge = may_parry = may_block = may_glance = false;
  }

  bool target_ready( player_t* t ) override
  {
    if ( t->health_percentage() > 20 )
      return false;
    return paladin_attack_t::target_ready( t );
  }
};

// Consecration: 8 s, et tick pr. sekund til alle fjender; tick-skaden er spellen 1280345 m.fl. (effekt 1 til alle,
// effekt 2 til de foerste 4 fjender; antaget at alle i simuleringen er blandt dem).
struct consecration_tick_t : public paladin_spell_t
{
  consecration_tick_t( paladin_t* player ) :
    paladin_spell_t( "consecration_tick", player,
                     player->rank.consecration.find_rank( RANK_MAX_USABLE, &spell_container_t::attack )->attack() )
  {
    background = dual = ground_aoe = true;
    aoe = -1;
    triggers_vengeance = false;
    if ( data().ok() )
    {
      base_dd_min = base_dd_max = data().effectN( 1 ).average( player, player->level() ) +
                                  data().effectN( 2 ).average( player, player->level() );
      spell_power_mod.direct = data().effectN( 2 ).sp_coeff();
    }
  }
};

struct consecration_t : public paladin_spell_t
{
  consecration_t( paladin_t* player, std::string_view options_str ) :
    paladin_spell_t( "consecration", player, player->rank.consecration.find_rank()->spell() )
  {
    parse_options( options_str );
    may_miss = may_crit = false;
    triggers_vengeance = false;
    dot_duration = data().duration();
    base_tick_time = 1_s;
    hasted_ticks = false;
    tick_action = new consecration_tick_t( player );
  }
};

// ==========================================================================
// Paladin
// ==========================================================================

action_t* paladin_t::create_action( util::string_view name, util::string_view options_str )
{
  if ( name == "auto_attack" )           return new auto_attack_t( this, options_str );
  if ( name == "seal_of_command" )       return new seal_of_command_t( this, options_str );
  if ( name == "seal_of_righteousness" ) return new seal_of_righteousness_t( this, options_str );
  if ( name == "judgement" )             return new judgement_t( this, options_str );
  if ( name == "holy_strike" )           return new holy_strike_t( this, options_str );
  if ( name == "hammer_of_wrath" )       return new hammer_of_wrath_t( this, options_str );
  if ( name == "consecration" )          return new consecration_t( this, options_str );

  return parse_player_effects_t::create_action( name, options_str );
}

void paladin_t::create_actions()
{
  active.seal_of_command_proc       = new seal_of_command_proc_t( this );
  active.seal_of_righteousness_proc = new seal_of_righteousness_proc_t( this );
  active.judgement_of_command       = new judgement_of_command_t( this );
  active.judgement_of_righteousness = new judgement_of_righteousness_t( this );
  // Til Sanctified Judgement (sealens manapris); bruges ikke i rotationen.
  active.seal_of_command_cast       = new seal_of_command_t( this, "" );
  active.seal_of_righteousness_cast = new seal_of_righteousness_t( this, "" );
  active.seal_of_command_cast->background = active.seal_of_righteousness_cast->background = true;

  seal_of_command_icd = get_cooldown( "seal_of_command_icd" );
  seal_of_command_icd->duration = rank.seal_of_command.find_rank()->spell()->internal_cooldown();

  parse_player_effects_t::create_actions();
}

void paladin_t::init_base_stats()
{
  // Classic-formlen for paladin (som warrior-modulet): AP = 3 x level - 20 + 2 x styrke.
  base.attack_power_per_strength = 2.0;

  parse_player_effects_t::init_base_stats();

  base.stats.attack_power = 3.0 * level() - 20.0;
}

void paladin_t::init_gains()
{
  parse_player_effects_t::init_gains();

  gain.sanctified_judgement = get_gain( "sanctified_judgement" );
}

void paladin_t::create_buffs()
{
  parse_player_effects_t::create_buffs();

  buff.seal_of_command = make_buff( this, "seal_of_command", rank.seal_of_command.find_rank()->spell() )
                           ->set_cooldown( 0_ms );
  buff.seal_of_righteousness = make_buff( this, "seal_of_righteousness", rank.seal_of_righteousness.find_rank()->spell() )
                                 ->set_cooldown( 0_ms );

  // Vengeance (20050): +rank-vaerdien % Physical og Holy pr. stak (talentkurven: 1/2/3), 5 stakke, 30 s.
  buff.vengeance = make_buff_fallback( talent.vengeance.ok(), this, "vengeance", find_spell( 20050 ) )
                     ->set_default_value( talent.vengeance->effectN( 1 ).percent() )
                     ->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER );
}

double paladin_t::composite_player_multiplier( school_e school ) const
{
  double m = parse_player_effects_t::composite_player_multiplier( school );

  if ( school == SCHOOL_PHYSICAL || school == SCHOOL_HOLY )
  {
    m *= 1.0 + buff.vengeance->check_stack_value();

    if ( talent.two_handed_weapon_specialization.ok() && two_hander() )
      m *= 1.0 + talent.two_handed_weapon_specialization->effectN( 1 ).percent();
  }

  return m;
}

void paladin_t::init_spells()
{
  parse_player_effects_t::init_spells();

  talent.seal_of_command      = find_talent_spell( talent_tree::CLASS, "Seal of Command" );
  talent.vengeance            = find_talent_spell( talent_tree::CLASS, "Vengeance" );
  talent.sanctified_judgement = find_talent_spell( talent_tree::CLASS, "Sanctified Judgement" );
  talent.improved_seals       = find_talent_spell( talent_tree::CLASS, "Improved Seals" );
  talent.two_handed_weapon_specialization = find_talent_spell( talent_tree::CLASS, "Two-Handed Weapon Specialization" );

  // kommentar = spell level
  rank.judgement = { this,
    spell_container_t().set_spell( 20271 ),  // 4
  };

  // spell = seal, attack = judgementens skade-spell (sealens effekt 3 -> Judgement of ... )
  rank.seal_of_righteousness = { this,
    spell_container_t().set_spell( 21084 ).set_attack( 20187 ),  // 1
    spell_container_t().set_spell( 20287 ).set_attack( 20280 ),  // 10
    spell_container_t().set_spell( 20288 ).set_attack( 20281 ),  // 18
    spell_container_t().set_spell( 20289 ).set_attack( 20282 ),  // 26
    spell_container_t().set_spell( 20290 ).set_attack( 20283 ),  // 34
    spell_container_t().set_spell( 20291 ).set_attack( 20284 ),  // 42
    spell_container_t().set_spell( 20292 ).set_attack( 20285 ),  // 50
    spell_container_t().set_spell( 20293 ).set_attack( 20286 ),  // 58
  };

  // Seal of Command er et talent i Forever (rank 1 = talentet; hoejere ranks kraever talentet).
  rank.seal_of_command = { this, talent.seal_of_command,
    spell_container_t().set_spell( 20375 ).set_attack( 20467 ),  // 20
    spell_container_t().set_spell( 20915 ).set_attack( 20963 ),  // 30
    spell_container_t().set_spell( 20918 ).set_attack( 20964 ),  // 40
    spell_container_t().set_spell( 20919 ).set_attack( 20965 ),  // 50
    spell_container_t().set_spell( 20920 ).set_attack( 20966 ),  // 60
  };

  rank.holy_strike = { this,
    spell_container_t().set_spell(   679 ),  // 6
    spell_container_t().set_spell(   678 ),  // 12
    spell_container_t().set_spell(  1866 ),  // 20
    spell_container_t().set_spell(   680 ),  // 28
    spell_container_t().set_spell(  2495 ),  // 36
    spell_container_t().set_spell(  5569 ),  // 44
    spell_container_t().set_spell( 10332 ),  // 52
    spell_container_t().set_spell( 10333 ),  // 60
  };

  rank.hammer_of_wrath = { this,
    spell_container_t().set_spell( 24275 ),  // 44
    spell_container_t().set_spell( 24274 ),  // 52
    spell_container_t().set_spell( 24239 ),  // 60
  };

  // spell = Consecration, attack = tick-skaden
  rank.consecration = { this,
    spell_container_t().set_spell( 26573 ).set_attack( 1280345 ),  // 20
    spell_container_t().set_spell( 20116 ).set_attack( 1280346 ),  // 30
    spell_container_t().set_spell( 20922 ).set_attack( 1280347 ),  // 40
    spell_container_t().set_spell( 20923 ).set_attack( 1280348 ),  // 50
    spell_container_t().set_spell( 20924 ).set_attack( 1280349 ),  // 60
  };

  parse_all_class_passives();
  // Talenternes passive effekter direkte fra spildata (som warrior, warlock, mage m.fl.): fx Improved Seals,
  // Conviction, Precision, Benediction, Improved Judgement, Divine Strength, Sacred Arbiter, Champion of the Light.
  parse_all_passive_talents();

  // Two-Handed Weapon Specialization (20111) er Modify Damage Done% med kravet "Requires weapon: Two-handed ...",
  // som simc ikke ser; den fjernes og laegges kun paa, naar main hand er et tohaandsvaaben.
  deregister_passive_spell( talent.two_handed_weapon_specialization );
}

void paladin_t::init_action_list()
{
  if ( equipped_weapons[ SLOT_MAIN_HAND ].type == WEAPON_NONE )
  {
    if ( !quiet )
      sim->error( "Player {} has no weapon equipped at the Main-Hand slot.", name() );
    quiet = true;
    return;
  }

  if ( !action_list_str.empty() )
  {
    parse_player_effects_t::init_action_list();
    return;
  }

  clear_action_priority_lists();
  paladin_apl::paladin( this );
  use_default_action_list = true;
  parse_player_effects_t::init_action_list();
}
}  // end namespace paladin

const module_t* module_t::paladin()
{
  static paladin::paladin_module_t m;
  return &m;
}
