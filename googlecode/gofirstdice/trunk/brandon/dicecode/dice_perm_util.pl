#!/usr/bin/perl

use strict;
use warnings;

use CGI;

my $cgi_var = new CGI;

print $cgi_var->header('text/html');
print $cgi_var->start_html('Dice Permutation Utility');


sub html_warn {
    my $warnstr = shift;

    #print '<p><b><font color="red">WARN: ', $warnstr, '</font></b></p>', "\n";
}


sub parse_dice {
    my $diceinref = shift;
    my $dicestrref = shift;
    my $nref = shift;

    my @dice_in = @{$diceinref};
    my @cleaner;


    for (my $i = 0; $i < scalar @dice_in; $i++) {
	$dice_in[$i] =~ s/^\s*//g;
	$dice_in[$i] =~ s/\s*$//g;

	$dice_in[$i] =~ s/[^a-z0-9:\s]/ /g;
	$dice_in[$i] =~ s/(?:die|dice)?\s*\d+\s*:\s*//ig;

	$dice_in[$i] =~ s/,/ /g;
	$dice_in[$i] =~ s/\s+/ /g;

	if ($dice_in[$i] =~ m/(?:[a-z0-9])/i) {
	    push @cleaner, $dice_in[$i];
	}
    }

    if (scalar @cleaner == 0) {
	return 1;
    }
  
    if (scalar @cleaner == 1) {
	# okay we probaby got aaaabbbcdeaaa or maybe 0 1 0 2 2 2 3
	# or some other similar shit like 0123, 4 5 9

	my %syms;
	my %order;
	
	while ($cleaner[0] =~ m/([a-z0-9])/ig) {
	    my $let = $1;

	    $syms{$let} = 1;
	}
	my $c = 0;
	foreach my $let (sort {$a cmp $b} keys %syms) {
	    $order{$let} = chr(ord('a') + $c);
	    $c++;
	}
	
	my $dstr = '';
	while ($cleaner[0] =~ m/([a-z0-9])/ig) {
	    my $let = $1;

	    $dstr .= $order{$let};
	}

	if (scalar keys %order > 15) {
	    return 1;
	}
	$$nref = scalar keys %order;
	$$dicestrref = $dstr;
	return 0;
    }

    if (scalar @cleaner > 1) {
	# We probably got something like
	# 0 3 6 ...
	# 1 4 7 ...
	# 2 5 8 ...

	my %syms;
	my %order;

	for (my $i = 0; $i < scalar @cleaner; $i++) {
	    while ($cleaner[$i] =~ m/(\d+)/g) {
		my $num = $1;
		
		if ($num > 1000000) {
		    return 1;
		}

		$syms{$num} = 1;
	    }
	}
	my $c = 0;
	foreach my $num (sort {$a <=> $b} keys %syms) {
	    $order{$num} = $c;
	    $c++;
	}

	my $dstr = '';
	foreach my $cnum (sort {$order{$a} <=> $order{$b}} keys %order) {
	    for (my $i = 0; $i < scalar @cleaner; $i++) {
		my $done = 0;
		while ($cleaner[$i] =~ m/(\d+)/g) {
		    my $num = $1;

		    if ($cnum == $num) {
			$dstr .= chr(ord('a') + $i);

			$done = 1;
			last;
		    }
		    if ($done == 1) {
			last;
		    }
		}
	    }
	}

	
	if (scalar @cleaner > 15) {
	    return 1;
	}
	if (scalar keys %order > 10000) {
	    return 1;
	}
	$$nref = scalar @cleaner;
	$$dicestrref = $dstr;
	return 0;
    }

}


sub find_perms {
    my $dstr = shift;
    my $permref = shift;

    my %dperms = ('' => 1);

    foreach my $let (split(//, $dstr)) {
	foreach my $pstr (sort keys %dperms) {

	    if (index($pstr, $let) < 0) {
		my $np = $pstr . $let;

		unless (exists $dperms{$np}) {
		    $dperms{$np} = 0;
		}

		$dperms{$np} += $dperms{$pstr};
	    }
	}
    }

    %{$permref} = %dperms;
}



########
if (defined $cgi_var->param('action_button')) {
    if ($cgi_var->param('action_button') eq 'Reset') {
	$cgi_var->delete_all();
    }
}

print $cgi_var->h4('Enter dice configuration<sup>*</sup>:');
print $cgi_var->start_form();
print '<p>', $cgi_var->textarea('input', '', 6, 96), '</p>';
print '<hr>', "\n";
print $cgi_var->submit('action_button', 'Process Dice');
print $cgi_var->submit('action_button', 'Reset');
print $cgi_var->end_form();
print '<hr>', "\n";

if (defined $cgi_var->param('input')) {
    my $input = $cgi_var->param('input');

    my @dice_in = split(/\n/, $input);

    #print 'lines: ', scalar @dice_in, "\n";
    my @clean_dice;
    my $dicestr;
    my $N;

    my $err = parse_dice(\@dice_in, \$dicestr, \$N);

    if ($err != 0) {
	print '<p><b>Unable to interpret input!</b></p>', "\n";
    }
    else {
	print '<p><b>Interpreted input in string form:</b></p>', "\n";
	print '<p>', $dicestr, '</p>', "\n";


	my @dienum;
	foreach my $let (split(//, $dicestr)) {
	    push @dienum, ord($let) - ord('a');
	}
	print '<p><b>Interpreted input in die-number form:</b></p>', "\n";
	print '<p>', join(', ', @dienum), '</p>', "\n";

	my @newdice;
	for (my $i = 0; $i < $N; $i++) {
	    push @newdice, [()];
	}
	my $c = 0;
	foreach my $let (split(//, $dicestr)) {
	    push @{$newdice[ord($let) - ord('a')]}, $c;
	    $c++;
	}
	print '<p><b>Interpreted input in matrix form:</b></p>', "\n";
	print '<textarea name="input"  rows="6" cols="96">', "\n";
	for (my $i = 0; $i < $N; $i++) {
	    print 'Die ', ($i + 1), ': ';
	    for (my $j = 0; $j < scalar @{$newdice[$i]}; $j++) {
		printf("%5d", $newdice[$i][$j]);
	    }
	    print "\n";
	}
	print '</textarea>', "\n";
	print '<hr />', "\n";

	if ($N <= 10) {
	    my %perms;
	    find_perms($dicestr, \%perms);
	
	    print '<p><b>Dice Permutation counts:</b></p>', "\n";
	    for (my $l = 0; $l <= $N; $l++) {
		print '<p>-- Length ', $l, ' permutations --</p>', "\n";
		foreach my $pstr (sort keys %perms) {
		    if (length $pstr == $l) {
			print '<p>"', $pstr, '" -&gt; ', $perms{$pstr},
			'</p>', "\n";
		    }
		}
	    }
	    
	    my @places;
	    for (my $i = 0; $i < $N; $i++) {
		$places[$i] = [(0) x $N];
	    }
	    foreach my $pstr (sort keys %perms) {
		if (length $pstr == $N) {
		    for (my $i = 0; $i < $N; $i++) {
			
			my $let = substr($pstr, $i, 1);
			my $dice = ord($let) - ord('a');
			
			$places[$dice][($N - 1) - $i] += $perms{$pstr};
		    }
		}
	    }
	    print '<p><b>Dice Place counts (for all ', $N,
	    ' dice only):</b></p>',
	    "\n";
	    print '<textarea name="input"  rows="6" cols="64">', "\n";
	    print 'Place: ';
	    for (my $i = 0; $i < $N; $i++) {
		printf("%9d", $i + 1);
	    }
	    print "\n";
	    for (my $i = 0; $i < $N; $i++) {
		print 'Die ', ($i + 1), ': ';
		for (my $j = 0; $j < $N; $j++) {
		    printf("%9d", $places[$i][$j]);
		}
		print "\n";
	    }
	    print '</textarea>', "\n";	
	}
	
	else {
	    print '<p><b>Sorry, that is too many dice!</b></p>', "\n";
	}
    }
    
}

print '<hr />', "\n";
print '<p><sup>*</sup>Only sane input formats are supported.</p>', "\n";
print $cgi_var->end_html();
