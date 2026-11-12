#include "class_modules/apl/rogue.hpp"

#include "player/action_priority_list.hpp"
#include "player/player.hpp"

namespace rogue_apl
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
//rogue_apl_start
void rogue( player_t* p )
{
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );

  // Aabning fra Stealth (kun foer kampen): Ambush med dolk i hovedhaanden, ellers Garrote
  default_->add_action( "ambush" );
  default_->add_action( "garrote" );
  default_->add_action( "auto_attack" );
  // Hold Slice and Dice og Rupture oppe
  default_->add_action( "slice_and_dice,if=buff.slice_and_dice.remains<1&combo_points>=2" );
  default_->add_action( "adrenaline_rush" );
  default_->add_action( "blade_flurry" );
  default_->add_action( "rupture,if=!ticking&combo_points>=5" );
  // Overskydende combo points paa Eviscerate, naar det ikke forsinker Slice and Dice
  default_->add_action( "cold_blood,if=combo_points>=5&(buff.slice_and_dice.remains>2|!learned.slice_and_dice)" );
  default_->add_action( "eviscerate,if=combo_points>=5&(buff.slice_and_dice.remains>2|!learned.slice_and_dice)" );
  // Builders: Shank for energi, Mutilate til 3, Sinister Strike til 5 combo points
  default_->add_action( "shank,if=energy<70" );
  default_->add_action( "mutilate,if=combo_points<=3" );
  default_->add_action( "sinister_strike,if=combo_points<5" );
}
//rogue_apl_end
}  // namespace rogue_apl
