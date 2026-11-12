#include "class_modules/apl/paladin.hpp"

#include "player/action_priority_list.hpp"
#include "player/player.hpp"

namespace paladin_apl
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
//paladin_apl_start
// AULA-RETTELSE: Retribution-basisrotation (Method/Warcraft Tavern): Seal of Command oppe (Seal of
// Righteousness hvis Seal of Command ikke er laert), Judgement paa nedkoeling, Hammer of Wrath under 20 %,
// Holy Strike paa nedkoeling, Consecration kun mod flere fjender og kun med over halv mana;
// auto-attack hele tiden (er manaen vaek, er auto-attack det eneste der sker).
// Holy: Holy Shock paa nedkoeling (med Divine Favor) over halv mana. Protection (talenterne afgoer): Righteous
// Fury, Holy Shield oppe over halv mana (kraever skjold), Swift Judgement og Consecration paa eet maal over 80 %
// mana. Mana-graenserne er vores valg (maalt: uden dem tager Holy Shock/Holy Shield manaen fra Seal of
// Righteousness og Judgement, og Holy/Protection faar lavere DPS end med Ret-rotationen).
void paladin( player_t* p )
{
  action_priority_list_t* precombat = p->get_action_priority_list( "precombat" );
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );

  precombat->add_action( "variable,name=prot,value=talent.templars_bulwark.enabled|talent.redoubt.enabled|talent.holy_shield.enabled|talent.swift_judgement.enabled|talent.shieldsworn.enabled" );
  precombat->add_action( "righteous_fury,if=variable.prot" );

  default_->add_action( "auto_attack" );
  default_->add_action( "seal_of_command,if=buff.seal_of_command.remains<1.5" );
  default_->add_action( "seal_of_righteousness,if=!buff.seal_of_command.up&buff.seal_of_righteousness.remains<1.5" );
  default_->add_action( "holy_shield,if=variable.prot&mana.pct>50" );
  default_->add_action( "judgement" );
  default_->add_action( "swift_judgement" );
  default_->add_action( "hammer_of_wrath,if=target.health.pct<20" );
  default_->add_action( "divine_favor,if=cooldown.holy_shock.ready&mana.pct>50" );
  default_->add_action( "holy_shock,if=mana.pct>50" );
  default_->add_action( "holy_strike" );
  default_->add_action( "consecration,if=active_enemies>1&mana.pct>50" );
  default_->add_action( "consecration,if=variable.prot&mana.pct>80" );
}
//paladin_apl_end
}  // namespace paladin_apl
