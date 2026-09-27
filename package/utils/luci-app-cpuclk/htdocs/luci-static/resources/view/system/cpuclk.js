'use strict';
'require view';
'require form';
'require fs';

return view.extend({
	load: function() {
		return L.resolveDefault(fs.read('/var/run/cpuclk'), '');
	},

	render: function(status) {
		var st = {}, m, s, o;

		(status || '').split('\n').forEach(function(line) {
			var i = line.indexOf('=');

			if (i > 0)
				st[line.substring(0, i)] = line.substring(i + 1);
		});

		m = new form.Map('cpuclk', _('CPU clock'),
			_('The CPU is rated for 1000 MHz. Higher clocks overclock it: the router may become unstable, and how far it goes differs from unit to unit. A new clock takes effect at the next boot.') + '<br />' +
			_('If the router does not start with the chosen clock, hold the WPS button while powering it on: it then starts at 1000 MHz, and the overclock is turned off.'));

		s = m.section(form.NamedSection, 'main', 'cpuclk');

		o = s.option(form.DummyValue, '_running', _('Current clock'));
		o.cfgvalue = function() {
			if (!st.running)
				return _('unknown');

			return _('%s MHz').format(st.running) +
				(st.failsafe == '1' ? ' (' + _('overclock turned off with the WPS button') + ')' : '');
		};

		o = s.option(form.ListValue, 'clock', _('Clock'));
		o.value('1000', _('%s MHz (stock)').format('1000'));
		['1050', '1100', '1150', '1200'].forEach(function(mhz) {
			o.value(mhz, _('%s MHz (overclock)').format(mhz));
		});
		o.default = '1000';

		return m.render();
	}
});
