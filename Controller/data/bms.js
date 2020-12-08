$(function () {
    $("#email").click(function() {$.ajax({
        type: "POST",
        url: "/email",
        success: function (data) {
            $("#savesuccess").show().delay(2000).fadeOut(500);
        },
        error: function (data) {
            $("#saveerror").show().delay(2000).fadeOut(500);
        }
    });});
    $("#menu > a").on('click',function (event) {
        $('#menu > a').removeClass("active");
        $(this).addClass("active");
        showContent($(this).attr("href"));
        event.preventDefault();
        }
    );
    $("#sensMenu").on('click',getSettings);
    $("#limitsMenu").on('click',getSettings);
    $("#battMenu").on('click',getSettings);
    $("#netMenu").on('click',getSettings);
    $(".error,.success").hide();
    $("#maxY,#minY").change(function () {
        config.options.scales.yAxes[0].ticks.max = parseFloat($("#maxY").val());
        config.options.scales.yAxes[0].ticks.min = parseFloat($("#minY").val());
        myChart.update();
    });
    showContent("cell");
    initChart();
    Setup();
    queryBMS();
});

var RELAY_TOTAL=0;
var nCells=0,nBanks=0;
var myChart,config;

function showContent(c) {
    $("#content > div").hide();
    $("#"+c).show();
}

function initChart() {
    config = {
        type: 'line',
        data: {
            labels: [ ],
            datasets: []
        },
        options: {
            responsive: true,
            title: {
                display: true,
                text: 'Cell Voltages'
            },
            tooltips: {
                mode: 'index',
                intersect: false,
            },
            hover: {
                mode: 'nearest',
                intersect: true
            },
            scales: {
                xAxes: [{
                    type: 'time',
                    time: {
                        parser: 'HH:mm:ss',
                        unit: 'second'
                    },
                    scaleLabel: {
                        display: true,
                        labelString: 'Date'
                    }
                }],
                yAxes: [{
                    display: true,
                    scaleLabel: {
                        display: true,
                        labelString: 'Volts'
                    },
                    ticks: { max: 3.7, min: 2.8 }
                }]
            }
        }
    };
    myChart = new Chart(document.getElementById('cellCan'), config);
}

var colors = [ '#e6194B', '#3cb44b', '#ffe119', '#4363d8', '#f58231', '#42d4f4', '#f032e6', '#fabed4', '#469990', '#dcbeff', '#9A6324', '#fffac8', '#800000', '#aaffc3', '#000075', '#a9a9a9', '#ffffff', '#000000' ];

function initCells() {
    config.data.labels = [];
    config.data.datasets = [];
    for (var i=0;i<nCells;i++) {
        config.data.datasets[i] = {
            label: 'Cell '+i,
            backgroundColor: colors[i],
            borderColor: colors[i],
            data: [ ],
            fill: false
        }
    }
    myChart.update();
    $("[cellval]").remove();
    for (var rel=0;rel<nCells;rel++) {
        var temp = $("#tempC").clone();
        temp.attr({id: "tempC"+rel});
        temp.find(".t").text('#'+rel+': T mV');
        temp.attr({cellval: true});
        temp.insertBefore("#tempC");
        var theA = temp.find("a");
        theA.attr("cell",rel);
        theA.click(function () {
            var r = $(this).attr("cell");
            $.ajax({
                type: "POST",
                url: "/dump",
                data: { cell: r},
                dataType:'json',
                success: function (data) {
                    var r = data.cell;
                    if (data.val == "off")
                        $("#tempC"+r+" .v").addClass("manoff");
                    else $("#tempC"+r+" .v").removeClass("manoff");
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
            return false;
         });
        temp.show();
    }
    for (var tn=0;tn<nCells;tn++) {
        var temp = $("#CellADC").clone();
        temp.attr({cellval: true});
        temp.attr({id: "CellADC"+tn});
        var t = temp.find("[for='cellCbCoef']");
        t.text("Cell "+tn+t.text());
        $.each(['cellCbCoef','cellCaddr','cellCmul','cellCdiv','cellCrange','cellCcur','celladdr','cellmul','celldiv','cellrange','cellcur'],function (index,value) {
            temp.find('#'+value).attr({id: value+tn, name: value+tn});
            temp.find("[for='"+value+"']").attr({for: value+tn});
        });
        temp.insertBefore("#CellADCPlace");
        temp.show();
    }
}

function getSettings() {
    $.getJSON("settings",
        function (data) {
            $("input[name='apName']").val(data.apName);
            $("input[name='email']").val(data.email);
            $("input[name='senderEmail']").val(data.senderEmail);
            $("input[name='senderServer']").val(data.senderServer);
            $("input[name='senderPort']").val(data.senderPort);
            $("input[name='senderSubject']").val(data.senderSubject);
            $("input[name='logEmail']").val(data.logEmail);
            $("input[name='doLogging']").prop("checked", data.doLogging);

            $("#Avg").val(data.Avg);
            $("#ConvTime").val(data.ConvTime);
            $("#BattAH").val(data.BattAH);
            $("#socLastAdj").html(data.socLastAdj);
            $("#socAvgAdj").html(data.socAvgAdj);
            $("#BatAHMeasured").html(data.BatAHMeasured);
            $("#MaxAmps").val(data.MaxAmps);
            $("#PVMaxAmps").val(data.PVMaxAmps);
            $("#ShuntUOhms").val(data.ShuntUOhms);
            $("#PVShuntUOhms").val(data.PVShuntUOhms);
            $("#nBanks").val(data.nBanks);
            $("#nCells").val(data.nCells);
            $("#ChargePct").val(data.ChargePct);
            $("#ChargePctRec").val(data.ChargePctRec);
            $("#FloatV").val(data.FloatV);
            $("#ChargeRate").val(data.ChargeRate);
            
            $.each(data.tempSettings,function (index,value) {
                $("#bCoef"+index).val(value.bCoef);
                $("#addr"+index).val(value.addr);
                $("#mul"+index).val(value.mul);
                $("#div"+index).val(value.div);
                $("#range"+index).val(value.range);
            });

            $.each(data.cellSettings,function (index,value) {
                $("#cellCbCoef"+index).val(value.bCoef);
                $("#cellCaddr"+index).val(value.cellCaddr);
                $("#cellCmul"+index).val(value.cellCmul);
                $("#cellCdiv"+index).val(value.cellCdiv);
                $("#cellCrange"+index).val(value.cellCrange);
                $("#celladdr"+index).val(value.celladdr);
                $("#cellmul"+index).val(value.cellmul);
                $("#celldiv"+index).val(value.celldiv);
                $("#cellrange"+index).val(value.cellrange);
            });

            $("#CurSOC").val('');
            $("#PollFreq").val(data.PollFreq);
            $("input[name='ssid'").val(data.ssid);

            $.each(data.limitSettings, function (index, value) {
                $("#" + index).val(value);
            });
            $("#useTempC").prop("checked", data.useTempC);
            $("#useCellC").prop("checked", data.useCellC);

            $.each(data.relaySettings, function (index, value) {
                $("#relayName" + index).val(value.name);
                setRelayType.call($("#relayType" + index).val(value.type));
                $("#relayTrip" + index).val(value.trip);
                $("#relayRec" + index).val(value.rec);
            });
    
        }).fail(function () { }
        );
    return true;
}

function formatNum(n,d) {
    return n.toLocaleString(undefined, { maximumFractionDigits: d, minimumFractionDigits: d });
}

function queryBMS() {
    $.getJSON("status", function (data) {
        if (data.debugstr) $("#debugstr").show().html(data.debugstr);
        else $("#debugstr").hide();

        if (data.RELAY_TOTAL && !RELAY_TOTAL) {
            setupRelays(data.RELAY_TOTAL);
            RELAY_TOTAL = data.RELAY_TOTAL;
        }
        for (var i=0;i<RELAY_TOTAL;i++) {
            var rn = data["relayName"+i];
            if (rn && rn.length) {
                $("#relayStatus"+i).show();
                $("#relayStatus"+i+" .t").html(rn);
                $("#relayStatus"+i+" .v").html(data["relayStatus"+i]);
                if (data["relayOff"+i] == "off")
                    $("#relayStatus"+i+" .v").addClass("manoff");
                else $("#relayStatus"+i+" .v").removeClass("manoff");
            } else $("#relayStatus"+i).hide();
        }
        if (data.watchDogHits) {
            $("#watchDogHits").show();
            $("#watchDogHits .v").html(data.watchDogHits);
        } else $("#watchDogHits").hide();

        var d = new Date(1000 * data.now);
        $("#timenow").html(d.toLocaleString());
        $("#version").html(data.version);

        $("#cellvolt").hide();
        if (data.maxCellVState || data.minCellVState)
            $("#cellvolt").show();
        val = "";
        if (data.maxCellVState)
            val = "over";
        if (data.minCellVState)
            val = val + " under";
        $("#cellvolt .v").html(val);

        var val = Number(data.packvolts);
        val = val/1000;
        if (data.maxPackVState || data.minPackVState)
            $("#packvolts .v").addClass("highlighted");
        else $("#packvolts .v").removeClass("highlighted");
        if (data.maxPackVState)
            val = "over " + val;
        if (data.minPackVState)
            val = "under " + val;
        $("#packvolts .v").html(formatNum(val,2));

        var pc = Number(data.packcurrent);
        var pvc = Number(data.pvcurrent);
        var lc = (pvc - pc)/1000;
        pc = pc / 1000;
        pvc = pvc / 1000;
        $("#loadcurrent .v").html(formatNum(lc,2));
        $("#packcurrent .v").html(formatNum(pc,2));
        $("#pvcurrent .v").html(formatNum(pvc,2));
        $("#soc .v").html(data.soc);
        if (data.socvalid)
            $("#soc .v").removeClass("manoff");
        else $("#soc .v").addClass("manoff");

        $("#celltemp").hide();
        if (data.maxCellCState || data.minCellCState)
            $("#celltemp").show();
        val = "";
        if (data.maxCellCState)
            val = "over";
        if (data.minCellCState)
            val = val + " under";
        $("#celltemp .v").html(val);

        val = data.temp1;
        if (data.maxPackCState || data.minPackCState)
            $("#temp1 .v").addClass("highlighted")
        else $("#temp1 .v").removeClass("highlighted")
        if (data.maxPackCState)
            val = "over " + val;
        if (data.minPackCState)
            val = "under " + val;
        $("#temp1 .v").html(val);
        val = data.temp2;
        if (val < -100)
            $("#temp2").hide();
        else {
            $("#temp2").show();
            $("#temp2 .v").html(data.temp2);
        }
        $('#fsr .v').html(data.fsr);
        $('#cur0').text(data.rawTemp1);
        $('#cur1').text(data.rawTemp2);
        $('#cur2').text(data.fsr);

        $("#uptime .v").html(data.uptime);

        if (data.nocells == true) {
            $("#nocells").show();
        } else {
            $("#nocells").fadeOut();
        }
        if (data.pkts) $("#pkts .v").html(data.pkts);

        $("#nocontroller").hide();

        if (data.nCells != nCells) {
            nCells = data.nCells;
            nBanks = data.nBanks;
            initCells();
        }
        config.data.labels.push(new Date());
        if (config.data.labels.length > 100)
            config.data.labels.shift();
        $.each(data.cells, function (index, value) { // does not handle multiple banks, multiple charts are needed
            var data = config.data.datasets[value.c].data;
            var valV = value.v / 1000.0;
            data.push(valV);
            if (data.length > 100)
                data.shift();
            $("#cellcur"+index).text(value.rv);
            $("#cellCcur"+index).text(value.rt);
            var tv = value.t + ' ' + formatNum(valV,3);
            var cell = $("#tempC"+value.c+" .v");
            cell.text(tv);
            if (value.d) cell.addClass("dumping");
            else cell.removeClass("dumping");
            myChart.update();
        });
        setTimeout(queryBMS, 2000);
    }).fail(function () {
        $("#nocontroller").show();
        setTimeout(queryBMS, 2000);
    });
}

function convertTemp(doC,t) {
    var t = $(t + " .v").html();
    if (doC) t = (t-32)*5/9;
    else t = t*9/5 + 32;
    $(t + " .v").html(Math.round(t));
}

function toggleTemp() {
    $.ajax({
        type: "GET",
        url: "/toggleTemp",
        success: function (data) {
            convertTemp(data.val,"#temp1");
            convertTemp(data.val,"#temp2");
            $("#savesuccess").show().delay(2000).fadeOut(500);
        },
        error: function (data) {
            $("#saveerror").show().delay(2000).fadeOut(500);
        }
    });
    return false;
}

function setRelayType() {
    var r = $(this).attr("relay");
    var val = $(this).val();
    if (val == "CP" || val == "LP")
        $("#relayDoSoC"+r).show();
    else $("#relayDoSoC"+r).hide();
}

function setupRelays(rt) {
    for (var rel=0;rel<rt;rel++) {
        var temp = $("#relay").clone();
        temp.attr({id: "relay"+rel});
        temp.find("[for='relayName']").text("J"+rel+":");
        $.each(['Name','DoSoC','Type','Trip','Rec'],function (index,value) {
            temp.find('#relay'+value).attr({id: "relay"+value+rel, name: "relay"+value+rel});
        });
        temp.find("[for]").each(function(index) {
            var val=$(this).attr("for");
            $(this).attr({for: val+rel});
        });
        var theTy = temp.find("#relayType" + rel);
        theTy.attr("relay",rel);
        theTy.change(setRelayType);
        temp.insertBefore("#relay");
        temp = $("#relayStatus").clone();
        temp.attr({id: "relayStatus"+rel});
        var theA = temp.find("a");
        theA.attr("relay",rel);
        theA.click(function () {
            var r = $(this).attr("relay");
            $.ajax({
                type: "POST",
                url: "/saveOff",
                data: { relay: r},
                dataType:'json',
                success: function (data) {
                    var r = data.relay;
                    if (data.val == "off")
                        $("#relayStatus"+r+" .v").addClass("manoff");
                    else $("#relayStatus"+r+" .v").removeClass("manoff");
                    $("#savesuccess").show().delay(2000).fadeOut(500);
                },
                error: function (data) {
                    $("#saveerror").show().delay(2000).fadeOut(500);
                }
            });
            return false;
         });
        temp.insertBefore("#relayStatus");
    }
    $("#relay").remove();
    $("#relayStatus").remove();
}

function Setup() {

    $("#loading").show();

    $("#temp1 a").click(toggleTemp);
    $("#temp2 a").click(toggleTemp);
    var LIMITNames = [['Volts (mV):','Temp (C):'],['Cell','Pack'],['Max','Min'],['Trip:','Rec:']];
    for (var l0=0;l0<LIMITNames[0].length;l0++) {
        for (var l1=0;l1<LIMITNames[1].length;l1++) {
            for (var l2=0;l2<LIMITNames[2].length;l2++) {
                var d = $("<div class='limits'></div>");
                for (var l3=0;l3<LIMITNames[3].length;l3++) {
                    var id=l0+""+l1+""+l2+""+l3;
                    var temp = $("#limit").clone();
                    var lab = temp.find("label");
                    if (l3) {
                        lab.html(LIMITNames[3][l3]);
                        lab.addClass('rec');
                    } else {
                        lab.addClass('field');
                        lab.html(LIMITNames[2][l2] + ' ' + LIMITNames[1][l1] + ' ' + LIMITNames[0][l0]);
                    }
                    var inp=temp.find("input");
                    inp.attr("for",id);
                    inp.attr("id",id);
                    inp.attr("name",id);
                    d.append(temp.children());
                }
                d.insertBefore("#limit");
            }
        }
    }
    $("#limit").remove();
    
    var TempNames = ['Temp','Temp 1'];
    for (var tn=0;tn<TempNames.length;tn++) {
        var temp = $("#Temps").clone();
        temp.attr({id: "Temps"+tn});
        var t = temp.find("[for='bCoef']");
        t.text(TempNames[tn]+t.text());
        $.each(['bCoef','addr','mul','div','range','cur'],function (index,value) {
            temp.find('#'+value).attr({id: value+tn, name: value+tn});
            temp.find("[for='"+value+"']").attr({for: value+tn});
        });
        temp.insertBefore("#Temps");
    }
    $("#Temps").remove();

    $('#kickWifi').click(function () {
        $.get("/kickwifi", function () { });
    });

    $("form").unbind('submit').submit(function (e) {
        e.preventDefault();
    
        $.ajax({
            type: $(this).attr('method'),
            url: $(this).attr('action'),
            data: $(this).serialize(),
            success: function (data) {
                $("#savesuccess").show().delay(2000).fadeOut(500);
            },
            error: function (data) {
                $("#saveerror").show().delay(2000).fadeOut(500);
            },
        });
    });
        
}