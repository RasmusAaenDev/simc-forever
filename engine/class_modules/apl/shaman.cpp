#include "class_modules/apl/shaman.hpp"

#include "player/action_priority_list.hpp"
#include "player/player.hpp"

namespace shaman_apl
{
std::string potion( const player_t* )
{
  return "disabled";
}

std::string flask( const player_t* )
{
  return "disabled";
}

std::string food( const player_t* )
{
  return "disabled";
}

std::string rune( const player_t* )
{
  return "disabled";
}

std::string temporary_enchant( const player_t* )
{
  return "disabled";
}

//shaman_apl_start
void shaman( player_t* p )
{
  action_priority_list_t* precombat = p->get_action_priority_list( "precombat" );
  action_priority_list_t* default_  = p->get_action_priority_list( "default" );

  action_priority_list_t* caster    = p->get_action_priority_list( "caster" );

  // AULA-RETTELSE: Elemental/Restoration (talenter i deres foerste raekke, uden Stormstrike) kaster
  // Lightning Bolt paa afstand; alle andre (Enhancement og shamaner uden talenter) er naerkaemper som foer.
  precombat->add_action( "variable,name=caster,value=!talent.stormstrike.enabled&(talent.convection.enabled|talent.concussion.enabled|talent.improved_healing_wave.enabled|talent.tidal_focus.enabled)" );
  precombat->add_action( "flametongue_weapon,if=!variable.caster" );
  precombat->add_action( "strength_of_earth_totem,if=!variable.caster" );
  precombat->add_action( "windfury_totem,if=!variable.caster" );
  precombat->add_action( "water_shield" );

  default_->add_action( "run_action_list,name=caster,if=variable.caster" );
  default_->add_action( "auto_attack" );
  default_->add_action( "strength_of_earth_totem,if=!buff.strength_of_earth.up" );
  default_->add_action( "windfury_totem,if=!buff.windfury_totem.up" );
  default_->add_action( "rage_of_the_farseer" );
  default_->add_action( "searing_totem,if=!ticking" );
  default_->add_action( "flame_shock,if=!ticking|remains<1" );
  default_->add_action( "stormstrike" );
  default_->add_action( "lightning_bolt,if=buff.maelstrom_weapon.stack=5" );
  default_->add_action( "earth_shock,if=dot.flame_shock.ticking|!action.flame_shock.enabled" );

  // Caster: mana-fase som hos priest (ingen wand): under 10 % mana kastes der ikke, til manaen er over
  // 40 % igen, saa femsekundsreglen ikke blokerer spirit-regen. Taersklerne er antagelser.
  caster->add_action( "water_shield" );
  caster->add_action( "variable,name=mana_phase,op=set,value=1,if=mana.pct<10" );
  caster->add_action( "variable,name=mana_phase,op=set,value=0,if=mana.pct>40" );
  caster->add_action( "searing_totem,if=!ticking" );
  caster->add_action( "lightning_mastery,if=!variable.mana_phase" );
  caster->add_action( "flame_shock,if=!variable.mana_phase&(!ticking|remains<1)" );
  caster->add_action( "chain_lightning,if=mana.pct>80|buff.clearcasting.up" );
  caster->add_action( "lightning_bolt,if=!variable.mana_phase|buff.clearcasting.up" );
}
//shaman_apl_end
}  // namespace shaman_apl
