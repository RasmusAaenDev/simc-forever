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

  // Aabning fra Stealth (foer kampen): Ambush med dolk i hovedhaanden, ellers Garrote. I kampen er de kun
  // klar efter Ghostly Strike (Subtlety-talent), som goer, at naeste Ambush/Garrote ikke kraever Stealth.
  // Efter Ghostly Strike: Garrote, naar den ikke tikker (maalt bedre end Ambush: bleed uden rustning med
  // Opportunity og Hemorrhage), ellers Ambush.
  default_->add_action( "ambush,if=!buff.ghostly_strike.up|dot.garrote.ticking" );
  default_->add_action( "garrote,if=!ticking" );
  default_->add_action( "auto_attack" );
  // Hold Slice and Dice og Rupture oppe
  default_->add_action( "slice_and_dice,if=buff.slice_and_dice.remains<1&combo_points>=2" );
  default_->add_action( "adrenaline_rush" );
  default_->add_action( "blade_flurry" );
  // Subtlety: Premeditation (+2 combo points) paa nedkoeling, Preparation nulstiller den
  default_->add_action( "premeditation,if=combo_points<=3" );
  default_->add_action( "preparation,if=cooldown.premeditation.remains>30" );
  default_->add_action( "rupture,if=!ticking&combo_points>=5" );
  // Overskydende combo points paa Eviscerate, naar det ikke forsinker Slice and Dice
  default_->add_action( "cold_blood,if=combo_points>=5&(buff.slice_and_dice.remains>2|!learned.slice_and_dice)" );
  default_->add_action( "eviscerate,if=combo_points>=5&(buff.slice_and_dice.remains>2|!learned.slice_and_dice)" );
  // Subtlety: Ghostly Strike aabner for Ambush/Garrote i kampen (hoejst 2 combo points, saa intet spildes)
  default_->add_action( "ghostly_strike,if=combo_points<=2" );
  // Builders: Shank for energi, Mutilate til 3, Hemorrhage (Subtlety), Backstab med dolk for Combat
  // (Puncturing Wounds) og Subtlety (Opportunity), ellers Sinister Strike til 5 combo points
  default_->add_action( "shank,if=energy<70" );
  default_->add_action( "mutilate,if=combo_points<=3" );
  default_->add_action( "hemorrhage,if=combo_points<5&!buff.ghostly_strike.up" );
  default_->add_action( "backstab,if=combo_points<5&(talent.puncturing_wounds.enabled|talent.opportunity.enabled)" );
  // Sinister Strike kun, naar Backstab ikke er valgt (ellers spares energien op til Backstab)
  default_->add_action( "sinister_strike,if=combo_points<5&!(action.backstab.enabled&(talent.puncturing_wounds.enabled|talent.opportunity.enabled))" );
}
//rogue_apl_end
}  // namespace rogue_apl
