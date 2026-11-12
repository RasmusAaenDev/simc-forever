#include "class_modules/apl/hunter.hpp"

#include "player/action_priority_list.hpp"
#include "player/player.hpp"

namespace hunter_apl
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

//hunter_apl_start
void hunter( player_t* p )
{
  action_priority_list_t* precombat = p->get_action_priority_list( "precombat" );
  action_priority_list_t* default_  = p->get_action_priority_list( "default" );

  precombat->add_action( "summon_pet" );
  precombat->add_action( "aspect_of_the_hawk" );
  precombat->add_action( "trueshot_aura" );
  precombat->add_action( "hunters_mark" );

  // Beast Mastery and Marksmanship shoot from range; Survival with melee talents fights in melee range (no melee
  // weaving). Actions out of range are skipped. Hunters run out of mana in a long fight, so the mana goes to Serpent
  // Sting and Arcane Shot (the most damage per mana) and is pooled for Bestial Wrath. Aimed Shot and Sniper Shot are
  // not in the default rotation: with the mana a hunter has they deal less than Arcane Shot.
  default_->add_action( "auto_shot" );
  default_->add_action( "auto_attack" );
  default_->add_action( "hunters_mark" );
  default_->add_action( "bestial_wrath" );
  default_->add_action( "rapid_fire" );
  default_->add_action( "variable,name=pool,value=talent.bestial_wrath.enabled&pet_active&cooldown.bestial_wrath.remains<3" );
  default_->add_action( "raptor_strike" );
  default_->add_action( "mongoose_bite" );
  default_->add_action( "strider_kick" );
  default_->add_action( "serpent_sting,if=!ticking&!variable.pool" );
  default_->add_action( "multi_shot,if=active_enemies>1&!variable.pool" );
  default_->add_action( "arcane_shot,if=dot.serpent_sting.ticking&!variable.pool" );
}
//hunter_apl_end
}  // namespace hunter_apl
